/*
 * Tencent is pleased to support the open source community by making wechat-matrix available.
 * Copyright (C) 2021 THL A29 Limited, a Tencent company. All rights reserved.
 * Licensed under the BSD 3-Clause License (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      https://opensource.org/licenses/BSD-3-Clause
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Created by Yves on 2020-03-11.
//

#include <dlfcn.h>
#include <unordered_map>
#include <cxxabi.h>
#include <sstream>
#include <iostream>
#include <xhook.h>
#include <cinttypes>
#include <regex>
#include <set>
#include <regex.h>
#include <Utils.h>
#include <fcntl.h>
#include <Backtrace.h>
#include "PthreadExt.h"
#include "PthreadHook.h"
#include "pthread.h"
#include "Log.h"
#include "JNICommon.h"
#include "cJSON.h"
#include "ReentrantPrevention.h"

#define ORIGINAL_LIB "libc.so"
#define TAG "Matrix.PthreadHook"

#define THREAD_NAME_LEN 16
#define PTHREAD_BACKTRACE_MAX_FRAMES MAX_FRAME_SHORT
#define PTHREAD_BACKTRACE_MAX_FRAMES_LONG MAX_FRAME_LONG_LONG
#define PTHREAD_BACKTRACE_FRAME_ELEMENTS_MAX_SIZE MAX_FRAME_NORMAL

typedef void *(*pthread_routine_t)(void *);

static volatile bool m_quicken_unwind = false;
static volatile size_t m_pthread_backtrace_max_frames =
        m_quicken_unwind ? PTHREAD_BACKTRACE_MAX_FRAMES_LONG : PTHREAD_BACKTRACE_MAX_FRAMES;

struct pthread_meta_t {
    pid_t tid;
    char *thread_name;
//    char  *parent_name;
    wechat_backtrace::BacktraceMode unwind_mode;

    uint64_t hash;

    wechat_backtrace::Backtrace native_backtrace;

    std::atomic<char *> java_stacktrace;

    pthread_meta_t() : tid(0),
                       thread_name(nullptr),
//                       parent_name(nullptr),
                       unwind_mode(wechat_backtrace::FramePointer),
                       hash(0),
                       native_backtrace(BACKTRACE_INITIALIZER(m_pthread_backtrace_max_frames)),
                       java_stacktrace(nullptr) {
    }

    ~pthread_meta_t() = default;

    pthread_meta_t(const pthread_meta_t &src) {
        tid = src.tid;
        thread_name = src.thread_name;
//        parent_name       = src.parent_name;
        unwind_mode = src.unwind_mode;
        hash = src.hash;
        native_backtrace = src.native_backtrace;
        java_stacktrace.store(src.java_stacktrace.load(std::memory_order_acquire),
                              std::memory_order_release);
    }
};

typedef struct {
    pthread_routine_t origin_func;
    void *origin_args;
} routine_wrapper_t;

struct regex_wrapper {
    const char *regex_str;
    regex_t regex;

    regex_wrapper(const char *regexStr, const regex_t &regex) : regex_str(regexStr), regex(regex) {}

    friend bool operator<(const regex_wrapper &left, const regex_wrapper &right) {
        return static_cast<bool>(strcmp(left.regex_str, right.regex_str));
    }
};

static std::mutex m_pthread_meta_mutex;
static std::timed_mutex m_java_stacktrace_mutex;
typedef std::lock_guard<std::mutex> pthread_meta_lock;

static std::map<pthread_t, pthread_meta_t> m_pthread_metas;
static std::set<pthread_t> m_filtered_pthreads;

static std::set<regex_wrapper> m_hook_thread_name_regex;

static pthread_key_t m_destructor_key;

static std::mutex m_subroutine_mutex;
static std::condition_variable m_subroutine_cv;

static std::set<pthread_t> m_pthread_routine_flags;

static void on_pthread_destroy(void *__specific);

void pthread_hook_init() {
    LOGD(TAG, "pthread_hook_init");

    if (!m_destructor_key) {
        pthread_key_create(&m_destructor_key, on_pthread_destroy);
    }

    rp_init();
}

void add_hook_thread_name(const char *__regex_str) {
//    std::regex regex(__regex_str);
    regex_t regex;
    if (0 != regcomp(&regex, __regex_str, REG_NOSUB)) {
        LOGE("PthreadHook", "regex compiled error: %s", __regex_str);
        return;
    }
    size_t len = strlen(__regex_str) + 1;
    char *p_regex_str = static_cast<char *>(malloc(len));
    strncpy(p_regex_str, __regex_str, len);
    regex_wrapper w_regex(p_regex_str, regex);
    m_hook_thread_name_regex.insert(w_regex);
    LOGD(TAG, "parent name regex: %s -> %s, len = %zu", __regex_str, p_regex_str, len);
}

static bool test_match_thread_name(pthread_meta_t &__meta) {
    for (const auto &w : m_hook_thread_name_regex) {
        if (__meta.thread_name && 0 == regexec(&w.regex, __meta.thread_name, 0, NULL, 0)) {
            LOGD(TAG, "test_match_thread_name: %s matches regex %s", __meta.thread_name,
                 w.regex_str);
            return true;
        } else {
            LOGD(TAG, "test_match_thread_name: %s NOT matches regex %s", __meta.thread_name,
                 w.regex_str);
        }
    }
    return false;
}

static inline bool
on_pthread_create_locked(const pthread_t __pthread, char *__java_stacktrace, bool quicken_unwind, pid_t __tid) {
    pthread_meta_lock meta_lock(m_pthread_meta_mutex);

    if (m_pthread_metas.count(__pthread)) {
        LOGD(TAG, "on_pthread_create: thread already recorded");
        return false;
    }

    pthread_meta_t &meta = m_pthread_metas[__pthread];

    meta.tid = __tid;

    // 如果还没 setname, 此时拿到的是父线程的名字, 在 setname 的时候有一次更正机会, 否则继承父线程名字
    // 如果已经 setname, 那么此时拿到的就是当前创建线程的名字
    meta.thread_name = static_cast<char *>(malloc(sizeof(char) * THREAD_NAME_LEN));
    if (0 != pthread_getname_ext(__pthread, meta.thread_name, THREAD_NAME_LEN)) {
        char temp_name[THREAD_NAME_LEN];
        snprintf(temp_name, THREAD_NAME_LEN, "tid-%d", pthread_gettid_np(__pthread));
        strncpy(meta.thread_name, temp_name, THREAD_NAME_LEN);
    }

    LOGD(TAG, "on_pthread_create: pthread = %ld, thread name: %s", __pthread, meta.thread_name);

    if (test_match_thread_name(meta)) {
        m_filtered_pthreads.insert(__pthread);
    }

    uint64_t native_hash = 0;
    uint64_t java_hash = 0;

    if (quicken_unwind) {
        meta.unwind_mode = wechat_backtrace::Quicken;
        wechat_backtrace::quicken_based_unwind(meta.native_backtrace.frames.get(),
                                               meta.native_backtrace.max_frames,
                                               meta.native_backtrace.frame_size);
    } else {
        meta.unwind_mode = wechat_backtrace::get_backtrace_mode();
        wechat_backtrace::unwind_adapter(meta.native_backtrace.frames.get(),
                                         meta.native_backtrace.max_frames,
                                         meta.native_backtrace.frame_size);
    }
    native_hash = hash_backtrace_frames(&(meta.native_backtrace));


    if (__java_stacktrace) {
        meta.java_stacktrace.store(__java_stacktrace);
        java_hash = hash_str(__java_stacktrace);
        LOGD(TAG, "on_pthread_create: java hash = %llu", (wechat_backtrace::ullint_t) java_hash);
    }

    if (native_hash || java_hash) {
        meta.hash = hash_combine(native_hash, java_hash);
    }

    return true;
}

static void notify_routine(const pthread_t __pthread) {
    std::lock_guard<std::mutex> routine_lock(m_subroutine_mutex);

    m_pthread_routine_flags.emplace(__pthread);
    LOGD(TAG, "notify waiting count : %zu", m_pthread_routine_flags.size());

    m_subroutine_cv.notify_all();
}

// notice: 在父线程回调此函数
static void on_pthread_create(const pthread_t __pthread) {
    const char *arch =
#ifdef __aarch64__
            "aarch64";
#elif defined __arm__
    "arm";
#endif
    LOGD(TAG, "+++++++ on_pthread_create, %s", arch);

    pid_t tid = pthread_gettid_np(__pthread);

    if (!rp_acquire()) {
        LOGD(TAG, "reentrant!!!");
        notify_routine(__pthread);
        return;
    }

    if (!m_quicken_unwind) {
        const size_t BUF_SIZE = 1024;
        char *java_stacktrace = static_cast<char *>(malloc(BUF_SIZE));
        strncpy(java_stacktrace, "(init stacktrace)", BUF_SIZE);
        if (m_java_stacktrace_mutex.try_lock_for(std::chrono::milliseconds(100))) {
            if (java_stacktrace) {
                get_java_stacktrace(java_stacktrace, BUF_SIZE);
            }
            m_java_stacktrace_mutex.unlock();
        } else {
            LOGE(TAG, "maybe reentrant!");
        }

        LOGD(TAG, "parent_tid: %d -> tid: %d", pthread_gettid_np(pthread_self()), tid);
        bool recorded = on_pthread_create_locked(__pthread, java_stacktrace, false, tid);

        if (!recorded && java_stacktrace) {
            free(java_stacktrace);
        }
    } else {
        LOGD(TAG, "parent_tid: %d -> tid: %d", pthread_gettid_np(pthread_self()), tid);
        on_pthread_create_locked(__pthread, nullptr, true, tid);
    }
//
    rp_release();
    notify_routine(__pthread);

    LOGD(TAG, "------ on_pthread_create end");
}

/**
 * ~~on_pthread_setname 有可能在 on_pthread_create 之前先执行~~
 * 在增加了 cond 之后, 必然后于 on_pthread_create 执行
 *
 * @param __pthread
 * @param __name
 */
static void on_pthread_setname(pthread_t __pthread, const char *__name) {
    if (NULL == __name) {
        LOGE(TAG, "setting name null");
        return;
    }

    const size_t name_len = strlen(__name);

    if (0 == name_len || name_len >= THREAD_NAME_LEN) {
        LOGE(TAG, "pthread name is illegal, just ignore. len(%s)", __name);
        return;
    }

    LOGD(TAG, "++++++++ pre on_pthread_setname tid: %d, %s", pthread_gettid_np(__pthread), __name);

    {
        pthread_meta_lock meta_lock(m_pthread_meta_mutex);

        if (!m_pthread_metas.count(__pthread)) { // always false
            // 到这里说明没有回调 on_pthread_create, setname 对 on_pthread_create 是可见的
            auto lost_thread_name = static_cast<char *>(malloc(sizeof(char) * THREAD_NAME_LEN));
            pthread_getname_ext(__pthread, lost_thread_name, THREAD_NAME_LEN);
            LOGE(TAG,
                 "on_pthread_setname: pthread hook lost: {%s} -> {%s}, maybe on_create has not been called",
                 lost_thread_name, __name);
            free(lost_thread_name);

            return;
        }

        // 到这里说明 on_pthread_create 已经回调了, 需要修正并检查新的线程名是否 match 正则

        pthread_meta_t &meta = m_pthread_metas.at(__pthread);

        LOGD(TAG, "on_pthread_setname: %s -> %s, tid:%d", meta.thread_name, __name, meta.tid);

        assert(meta.thread_name != nullptr);
        strncpy(meta.thread_name, __name, THREAD_NAME_LEN);

        bool parent_match = m_filtered_pthreads.count(__pthread) != 0;

        // 如果新线程名不 match, 但父线程名 match, 说明需要从 filter 集合中移除
        if (!test_match_thread_name(meta) && parent_match) {
            m_filtered_pthreads.erase(__pthread);
            LOGD(TAG, "--------------------------");
            return;
        }

        // 如果新线程 match, 但父线程名不 match, 说明需要添加仅 filter 集合
        if (test_match_thread_name(meta) && !parent_match) {
            m_filtered_pthreads.insert(__pthread);
            LOGD(TAG, "--------------------------");
            return;
        }
    }

    // 否则, 啥也不干 (都 match, 都不 match)
    LOGD(TAG, "--------------------------");
}

static inline void before_routine_start() {
    LOGI(TAG, "before_routine_start");
    std::unique_lock<std::mutex> routine_lock(m_subroutine_mutex);

    pthread_t self_thread = pthread_self();

    m_subroutine_cv.wait(routine_lock, [&self_thread] {
        return m_pthread_routine_flags.count(self_thread);
    });

    LOGI(TAG, "before_routine_start: create ready, just continue, waiting count : %zu",
         m_pthread_routine_flags.size());

    m_pthread_routine_flags.erase(self_thread);
}


static inline void pthread_dump_impl(FILE *__log_file) {
    if (!__log_file) {
        LOGE(TAG, "open file failed");
        return;
    }

    for (auto &i: m_pthread_metas) {
        auto &meta = i.second;
        LOGD(TAG, "========> RETAINED PTHREAD { name : %s, tid: %d }", meta.thread_name, meta.tid);
        flogger(__log_file, "========> RETAINED PTHREAD { name : %s, tid: %d }\n",
                meta.thread_name, meta.tid);
        std::stringstream stack_builder;

        if (meta.native_backtrace.frame_size == 0) {
            continue;
        }

        if (meta.unwind_mode == wechat_backtrace::FramePointer) {
            LOGD(TAG, "native stacktrace:");
            flogger(__log_file, "native stacktrace:\n");

            auto frame_detail_lambda = [&__log_file](wechat_backtrace::FrameDetail detail) -> void {
                int status = 0;
                char *demangled_name = abi::__cxa_demangle(detail.function_name, nullptr, 0,
                                                           &status);

                LOGD(TAG, "  #pc %"
                        PRIxPTR
                        " %s (%s)",
                     detail.rel_pc,
                     demangled_name ? demangled_name : "(null)",
                     detail.map_name);
                flogger(__log_file, "  #pc %" PRIxPTR " %s (%s)\n", detail.rel_pc,
                        demangled_name ? demangled_name : "(null)", detail.map_name);

                free(demangled_name);
            };

            wechat_backtrace::restore_frame_detail(meta.native_backtrace.frames.get(),
                                                   meta.native_backtrace.frame_size,
                                                   frame_detail_lambda);

            LOGD(TAG, "java stacktrace:\n%s", meta.java_stacktrace.load(std::memory_order_acquire));
            flogger(__log_file, "java stacktrace:\n%s\n",
                    meta.java_stacktrace.load(std::memory_order_acquire));
        } else if (meta.unwind_mode == wechat_backtrace::Quicken) {

            LOGD(TAG, "native stacktrace:");
            flogger(__log_file, "native stacktrace:\n");

            size_t elements_size = 0;
            const size_t max_elements = PTHREAD_BACKTRACE_FRAME_ELEMENTS_MAX_SIZE;
            wechat_backtrace::FrameElement stacktrace_elements[max_elements];

            get_stacktrace_elements(meta.native_backtrace.frames.get(),
                                    meta.native_backtrace.frame_size,
                                    true, stacktrace_elements,
                                    max_elements, elements_size);

            for (size_t i = 0; i < elements_size; i++) {
                std::string data;
                wechat_backtrace::quicken_frame_format(stacktrace_elements[i], i, data);
                LOGD(TAG, "%s", data.c_str());
                flogger(__log_file, data.c_str());

            }

            LOGD(TAG, "java stacktrace:\n%s", meta.java_stacktrace.load(std::memory_order_acquire));
            flogger(__log_file, "java stacktrace:\n%s\n",
                    meta.java_stacktrace.load(std::memory_order_acquire));
        }
    }
}

void pthread_dump(const char *__path) {
    LOGD(TAG,
         ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pthread dump begin <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<");
    pthread_meta_lock meta_lock(m_pthread_meta_mutex);

    FILE *log_file = fopen(__path, "w+");
    LOGD(TAG, "pthread dump path = %s", __path);

    pthread_dump_impl(log_file);

    fclose(log_file);

    LOGD(TAG,
         ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pthread dump end <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<");
}


static inline void pthread_dump_json_impl(FILE *__log_file) {

    LOGD(TAG, "pthread dump waiting count: %zu", m_pthread_routine_flags.size());

    std::map<uint64_t, std::vector<pthread_meta_t>> pthread_metas_by_hash;

    for (auto &i : m_filtered_pthreads) {
        auto &meta = m_pthread_metas[i];
        if (meta.hash) {
            auto &hash_bucket = pthread_metas_by_hash[meta.hash];
            hash_bucket.emplace_back(meta);
        }
    }

    char *json_str = NULL;
    cJSON *threads_arr = NULL;

    cJSON *json_obj = cJSON_CreateObject();

    if (!json_obj) {
        goto err;
    }

    threads_arr = cJSON_AddArrayToObject(json_obj, "PthreadHook");

    if (!threads_arr) {
        goto err;
    }

    for (auto &i : pthread_metas_by_hash) {
        auto &hash = i.first;
        auto &metas = i.second;

        cJSON *hash_obj = cJSON_CreateObject();

        if (!hash_obj) {
            goto err;
        }

        cJSON_AddStringToObject(hash_obj, "hash", std::to_string(hash).c_str());
        assert(!metas.empty());

        auto meta = metas.front();
        auto front_backtrace = &metas.front().native_backtrace;
        if (meta.unwind_mode == wechat_backtrace::FramePointer) {
            std::stringstream stack_builder;

            auto frame_detail_lambda = [&stack_builder](
                    wechat_backtrace::FrameDetail detail) -> void {
                char *demangled_name = nullptr;

                int status = 0;
                demangled_name = abi::__cxa_demangle(detail.function_name, nullptr, nullptr,
                                                     &status);

                stack_builder << "#pc " << std::hex << detail.rel_pc << " "
                              << (demangled_name ? demangled_name : "(null)")
                              << " ("
                              << detail.map_name
                              << ");";

                LOGE(TAG, "#pc %p %s %s", (void *) detail.rel_pc, demangled_name,
                     detail.map_name);

                if (demangled_name) {
                    free(demangled_name);
                }
            };

            wechat_backtrace::restore_frame_detail(
                    front_backtrace->frames.get(), front_backtrace->frame_size, frame_detail_lambda);

            LOGE(TAG, "-------------------");
            cJSON_AddStringToObject(hash_obj, "native", stack_builder.str().c_str());

            const char *java_stacktrace = metas.front().java_stacktrace.load(
                    std::memory_order_acquire);
            cJSON_AddStringToObject(hash_obj, "java", java_stacktrace ? java_stacktrace : "");
        } else if (meta.unwind_mode == wechat_backtrace::Quicken) {
            std::stringstream native_stack_builder;
            std::stringstream java_stack_builder;

            size_t elements_size = 0;
            const size_t max_elements = PTHREAD_BACKTRACE_FRAME_ELEMENTS_MAX_SIZE;
            wechat_backtrace::FrameElement stacktrace_elements[max_elements];
            get_stacktrace_elements(front_backtrace->frames.get(),
                                    front_backtrace->frame_size,
                                    true, stacktrace_elements,
                                    max_elements, elements_size);
            bool found_java = false;
            LOGI(TAG, "Pthread using quicken: elements_size %zu, frames_size %zu", elements_size, front_backtrace->frame_size);
            for (size_t i = 0; i < elements_size; i++) {
                auto element = &stacktrace_elements[i];
                LOGI(TAG, "elements #%zu: %llx %s %d", i, element->rel_pc, element->function_name.c_str(), element->maybe_java);
                if (!found_java) found_java = element->maybe_java;
                if (!found_java) {
                    native_stack_builder << "#pc " << std::hex << element->rel_pc << " "
                                  << (!element->function_name.empty() ? element->function_name : "(null)")
                                  << " ("
                                  << element->map_name
                                  << ");";
                } else {
                    java_stack_builder << (!element->function_name.empty() ? element->function_name : "(null)")
                                         << " (+"
                                         << element->function_offset
                                         << ");";
                }
            }

            LOGE(TAG, "-------------------");

            cJSON_AddStringToObject(hash_obj, "native", native_stack_builder.str().c_str());
            cJSON_AddStringToObject(hash_obj, "java", java_stack_builder.str().c_str());
        }

        cJSON_AddStringToObject(hash_obj, "count", std::to_string(metas.size()).c_str());

        cJSON *same_hash_metas_arr = cJSON_AddArrayToObject(hash_obj, "threads");

        if (!same_hash_metas_arr) {
            goto err;
        }

        for (auto &meta: metas) {
            cJSON *meta_obj = cJSON_CreateObject();

            if (!meta_obj) {
                goto err;
            }

            cJSON_AddStringToObject(meta_obj, "tid", std::to_string(meta.tid).c_str());
            cJSON_AddStringToObject(meta_obj, "name", meta.thread_name);

            cJSON_AddItemToArray(same_hash_metas_arr, meta_obj);
        }

        cJSON_AddItemToArray(threads_arr, hash_obj);

        LOGD(TAG, "%s", cJSON_Print(hash_obj));
    }

    json_str = cJSON_PrintUnformatted(json_obj);

    cJSON_Delete(json_obj);

    flogger(__log_file, "%s", json_str);
    cJSON_free(json_str);
    return;

    err:
    LOGD(TAG, "ERROR: create cJSON object failed");
    cJSON_Delete(json_obj);

    return;
}

void enable_quicken_unwind(const bool enable) {
    m_quicken_unwind = enable;
    m_pthread_backtrace_max_frames =
            m_quicken_unwind ? PTHREAD_BACKTRACE_MAX_FRAMES_LONG : PTHREAD_BACKTRACE_MAX_FRAMES;
}

void pthread_dump_json(const char *path) {

    LOGD(TAG,
         ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pthread dump json begin <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<");
    pthread_meta_lock meta_lock(m_pthread_meta_mutex);

    FILE *log_file = fopen(path, "w+");
    LOGD(TAG, "pthread dump path = %s", path);

    if (log_file) {
        pthread_dump_json_impl(log_file);
        fclose(log_file);
    }

    LOGD(TAG,
         ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pthread dump json end <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<");
}

void pthread_hook_on_dlopen(const char *file_name, bool *maps_refreshed) {
    LOGD(TAG, "pthread_hook_on_dlopen");
    if (!*maps_refreshed) {
        *maps_refreshed = true;
        pthread_meta_lock meta_lock(m_pthread_meta_mutex);
        wechat_backtrace::notify_maps_changed();
    }
    LOGD(TAG, "pthread_hook_on_dlopen end");
}

static void on_pthread_destroy(void *specific) {
    LOGD(TAG, "on_pthread_destroy++++");
    pthread_meta_lock meta_lock(m_pthread_meta_mutex);

    pthread_t destroying_thread = pthread_self();

    if (!m_pthread_metas.count(destroying_thread)) {
        LOGD(TAG, "on_pthread_destroy: thread not found");
        return;
    }

    pthread_meta_t &meta = m_pthread_metas.at(destroying_thread);
    LOGD(TAG, "removing thread {%ld, %s, %d}", destroying_thread, meta.thread_name, meta.tid);

    free(meta.thread_name);

    char *java_stacktrace = meta.java_stacktrace.load(std::memory_order_acquire);
    if (java_stacktrace) {
        free(java_stacktrace);
    }

    m_pthread_metas.erase(destroying_thread);
    m_filtered_pthreads.erase(destroying_thread);

    LOGD(TAG, "specific %c", *(char *) specific);

    free(specific);

    LOGD(TAG, "on_pthread_destroy end----");
}

static void *pthread_routine_wrapper(void *arg) {

    auto *specific = (char *) malloc(sizeof(char));
    *specific = 'P';

    pthread_setspecific(m_destructor_key, specific);

    before_routine_start();

    auto *args_wrapper = (routine_wrapper_t *) arg;
    void *ret = args_wrapper->origin_func(args_wrapper->origin_args);
    free(args_wrapper);

    return ret;
}

DEFINE_HOOK_FUN(int, pthread_create, pthread_t *pthread_ptr, pthread_attr_t const *attr,
                void *(*start_routine)(void *), void *arg) {
    auto *args_wrapper = (routine_wrapper_t *) malloc(sizeof(routine_wrapper_t));
    args_wrapper->origin_func = start_routine;
    args_wrapper->origin_args = arg;

    CALL_ORIGIN_FUNC_RET(int, ret, pthread_create, pthread_ptr, attr, pthread_routine_wrapper,
                         args_wrapper);

    if (0 == ret) {
        on_pthread_create(*pthread_ptr);
    }

    return ret;
}

DEFINE_HOOK_FUN(int, pthread_setname_np, pthread_t
        pthread, const char *name) {
    CALL_ORIGIN_FUNC_RET(int, ret, pthread_setname_np, pthread, name);
    if (0 == ret) {
        on_pthread_setname(pthread, name);
    }
    return ret;
}

#undef ORIGINAL_LIB
