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
// Created by Yves on 2019-08-08.
//

#include <malloc.h>
#include <unistd.h>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <list>
#include <map>
#include <memory>
#include <random>
#include <xhook.h>
#include <sstream>
#include <cxxabi.h>
#include <sys/mman.h>
#include <mutex>
#include <condition_variable>
#include <shared_mutex>
#include <cJSON.h>
#include <Log.h>
#include "MemoryHookFunctions.h"
#include "Utils.h"
#include "unwindstack/Unwinder.h"
#include "ThreadPool.h"
#include "BacktraceDefine.h"
#include "MemoryHookMetas.h"
#include "MemoryHook.h"

#define MEMHOOK_BACKTRACE_MAX_FRAMES MAX_FRAME_SHORT

static memory_meta_container m_memory_meta_container;

static bool is_stacktrace_enabled = false;
static bool is_caller_sampling_enabled = false;

static size_t m_sample_size_min = 0;
static size_t m_sample_size_max = 0;
static double m_sampling = 1;

static size_t m_stacktrace_log_threshold;

void enable_stacktrace(bool enable) {
    is_stacktrace_enabled = enable;
}

void set_stacktrace_log_threshold(size_t threshold) {
    m_stacktrace_log_threshold = threshold;
}

void set_sample_size_range(size_t min, size_t max) {
    m_sample_size_min = min;
    m_sample_size_max = max;
}

void set_sampling(double sampling) {
    m_sampling = sampling;
}

void enable_caller_sampling(bool enable) {
    is_caller_sampling_enabled = enable;
}

static inline void
decrease_stack_size(std::map<uint64_t, stack_meta_t> &stack_metas,
                    const ptr_meta_t &meta) {
    LOGI(TAG, "calculate_stack_size");

}

void memory_hook_init() {
    LOGI(TAG, "memory_hook_init");
}

static inline bool should_do_unwind(size_t byte_count, void *caller) {
    if (!is_caller_sampling_enabled) {

        return ((m_sample_size_min == 0 || byte_count >= m_sample_size_min)
                && (m_sample_size_max == 0 || byte_count <= m_sample_size_max)
                && rand() <= m_sampling * RAND_MAX);

    }

    // TODO caller sampling
    return false;
}

static inline void on_acquire_memory(void *caller,
                                     void *ptr,
                                     size_t byte_count,
                                     bool is_mmap) {
//    NanoSeconds_Start(alloc_begin);
////    NanoSeconds_End(alloc_cost, alloc_begin);
////    LOGD(TAG, "alloc stamp cost %lld", alloc_cost);

    if (!ptr) {
        LOGE(TAG, "on_acquire_memory: invalid pointer");
        return;
    }

    wechat_backtrace::Backtrace backtrace;
    uint64_t stack_hash = 0;
    if (is_stacktrace_enabled && should_do_unwind(byte_count, caller)) {
        backtrace = BACKTRACE_INITIALIZER(MEMHOOK_BACKTRACE_MAX_FRAMES);
        wechat_backtrace::unwind_adapter(backtrace.frames.get(), backtrace.max_frames,
                                         backtrace.frame_size);
        stack_hash = hash_backtrace_frames(&backtrace);
        assert(stack_hash != 0);
    }

    m_memory_meta_container.insert(ptr,
                                   stack_hash,
                                   [&](ptr_meta_t *ptr_meta, stack_meta_t *stack_meta) {
                                       ptr_meta->ptr     = ptr;
                                       ptr_meta->size    = byte_count;
                                       ptr_meta->caller  = caller;
                                       ptr_meta->is_mmap = is_mmap;

                                       if (!stack_meta) {
                                           return;
                                       }

                                       stack_meta->size += byte_count;
                                       if (!stack_meta->backtrace.frames) { // 相同的堆栈只记录一个
                                           stack_meta->backtrace = backtrace;
                                           stack_meta->caller = caller;
                                       }
                                   });

//    NanoSeconds_End(alloc_cost, alloc_begin);
//    LOGD(TAG, "alloc cost %lld", alloc_cost);
}

static inline void on_release_memory(void *ptr, bool is_mmap) {
    if (!ptr) {
        LOGE(TAG, "on_release_memory: invalid pointer");
        return;
    }
//    NanoSeconds_Start(release_begin);

    m_memory_meta_container.erase(ptr);
//    NanoSeconds_End(release_cost, release_begin);
//    LOGD(TAG, "release cost %lld", release_cost);
}

void on_alloc_memory(void *caller, void *ptr, size_t byte_count) {
    on_acquire_memory(caller, ptr, byte_count, false);
}

void on_free_memory(void *ptr) {
    on_release_memory(ptr, true);
}

void on_mmap_memory(void *caller, void *ptr, size_t byte_count) {
    on_acquire_memory(caller, ptr, byte_count, true);
}

void on_munmap_memory(void *ptr) {
    on_release_memory(ptr, true);
}

/**
 * 区分 native heap 和 mmap 的 caller 和 stack
 * @param heap_caller_metas
 * @param mmap_caller_metas
 * @param heap_stack_metas
 * @param mmap_stack_metas
 */
static inline size_t collect_metas(std::map<void *, caller_meta_t> &heap_caller_metas,
                                   std::map<void *, caller_meta_t> &mmap_caller_metas,
                                   std::map<uint64_t, stack_meta_t> &heap_stack_metas,
                                   std::map<uint64_t, stack_meta_t> &mmap_stack_metas) {
    LOGD(TAG, "collect_metas");

    size_t ptr_meta_size = 0;

    m_memory_meta_container.for_each(
            [&](const void *ptr, ptr_meta_t *meta, stack_meta_t *stack_meta) {

                auto &dest_caller_metes =
                             meta->is_mmap ? mmap_caller_metas : heap_caller_metas;
                auto &dest_stack_metas  = meta->is_mmap ? mmap_stack_metas : heap_stack_metas;

                if (meta->caller) {
                    caller_meta_t &caller_meta = dest_caller_metes[meta->caller];
                    caller_meta.pointers.insert(ptr);
                    caller_meta.total_size += meta->size;
                }

                if (stack_meta) {
                    auto &dest_stack_meta = dest_stack_metas[meta->stack_hash];
                    dest_stack_meta.backtrace = stack_meta->backtrace;
                    // 没错, 这里的确使用 ptr_meta 的 size, 因为是在遍历 ptr_meta, 因此原来 stack_meta 的 size 仅起引用计数作用
                    dest_stack_meta.size += meta->size;
                    dest_stack_meta.caller     = stack_meta->caller;
                }

                ptr_meta_size++;
            });

    LOGD(TAG, "collect_metas done");
    return ptr_meta_size;
}

static inline void dump_callers(FILE *log_file,
                                cJSON *json_size_arr,
//                                const std::multimap<void *, ptr_meta_t> &ptr_metas,
                                std::map<void *, caller_meta_t> &caller_metas) {

    if (caller_metas.empty()) {
        LOGI(TAG, "dump_callers: nothing dump");
        return;
    }

    LOGD(TAG, "dump_callers: count = %zu", caller_metas.size());
    flogger(log_file, "dump_callers: count = %zu\n", caller_metas.size());

    std::unordered_map<std::string, size_t> caller_alloc_size_of_so;
    std::unordered_map<std::string, std::map<size_t, size_t>> same_size_count_of_so;

    LOGD(TAG, "caller so begin");
    // 按 so 聚类
    for (auto &i : caller_metas) {
        auto caller = i.first;
        auto caller_meta = i.second;

        Dl_info dl_info;
        dladdr(caller, &dl_info);

        if (!dl_info.dli_fname) {
            continue;
        }
        caller_alloc_size_of_so[dl_info.dli_fname] += caller_meta.total_size;

        // 按 size 聚类
        for (auto pointer : caller_meta.pointers) {
            m_memory_meta_container.get(pointer,
                                        [&same_size_count_of_so, &dl_info](ptr_meta_t &meta) {
                                            same_size_count_of_so[dl_info.dli_fname][meta.size]++;
                                        });
        }
    }

    // 排序 so
    std::multimap<size_t, std::string> result_sort_by_size;

    std::transform(caller_alloc_size_of_so.begin(),
                   caller_alloc_size_of_so.end(),
                   std::inserter(result_sort_by_size, result_sort_by_size.begin()),
                   [](const std::pair<std::string, size_t> &src) {
                       return std::pair<size_t, std::string>(src.second, src.first);
                   });

    size_t caller_total_size = 0;

    for (auto i = result_sort_by_size.rbegin(); i != result_sort_by_size.rend(); ++i) {
        auto &so_name = i->second;
        auto &so_size = i->first;
        LOGD(TAG, "so = %s, caller alloc size = %zu", i->second.c_str(), i->first);
        cJSON *so_size_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(so_size_obj, "so", so_name.c_str());
        cJSON_AddStringToObject(so_size_obj, "size", std::to_string(so_size).c_str());
        cJSON_AddItemToArray(json_size_arr, so_size_obj);
        flogger(log_file, "caller alloc size = %10zu b, so = %s\n", so_size, so_name.c_str());

        caller_total_size += so_size;

        auto count_of_size = same_size_count_of_so[so_name];
        std::multimap<size_t, std::pair<size_t, size_t>> result_sort_by_mul;
        std::transform(count_of_size.begin(),
                       count_of_size.end(),
                       std::inserter(result_sort_by_mul, result_sort_by_mul.begin()),
                       [](std::pair<size_t, size_t> src) {
                           return std::pair<size_t, std::pair<size_t, size_t >>(
                                   src.first * src.second,
                                   std::pair<size_t, size_t>(src.first, src.second));
                       });

        int lines = 20; // fixme hard coding
        LOGD(TAG, "top %d (size * count):", lines);
        flogger(log_file, "top %d (size * count):\n", lines); // fixme using json

        for (auto sc = result_sort_by_mul.rbegin();
             sc != result_sort_by_mul.rend() && lines; ++sc, --lines) {
            auto size = sc->second.first;
            auto count = sc->second.second;
            LOGD(TAG, "   size = %10zu b, count = %zu", size, count);
            flogger(log_file, "   size = %10zu b, count = %zu\n", size, count);
        }
    }

    LOGD(TAG, "\n---------------------------------------------------");
    flogger(log_file, "\n---------------------------------------------------\n");
    LOGD(TAG, "| caller total size = %zu b", caller_total_size);
    flogger(log_file, "| caller total size = %zu b\n", caller_total_size);
    LOGD(TAG, "---------------------------------------------------\n");
    flogger(log_file, "---------------------------------------------------\n\n");
}

//static inline void dump_debug_stacks() {
//    for (auto map_it : m_debug_lost_ptr_stack) {
//        auto ptr        = map_it.first;
//        auto stack_meta = map_it.second;
//
//        LOGD(TAG, "LOST ptr %p, stack = ", ptr);
//        for (auto &it : *stack_meta.p_stacktraces) {
//            Dl_info stack_info = {nullptr};
//            int     success    = dladdr((void *) it.pc, &stack_info);
//
//            std::stringstream stack_builder;
//
//            char *demangled_name = nullptr;
//            if (success > 0) {
//                int status = 0;
//                demangled_name = abi::__cxa_demangle(stack_info.dli_sname, nullptr, 0, &status);
//            }
//
//            stack_builder << "      | "
//                          << "#pc " << std::hex << it.rel_pc << " "
//                          << (demangled_name ? demangled_name : "(null)")
//                          << " ("
//                          << (success && stack_info.dli_fname ? stack_info.dli_fname : "(null)")
//                          << ")"
//                          << std::endl;
//
//            LOGD(TAG, "%s", stack_builder.str().c_str());
//
//            if (demangled_name) {
//                free(demangled_name);
//            }
//        }
//    }
//
//    m_debug_lost_ptr_stack.clear();
//}

struct stack_dump_meta_t {
    size_t size;
    std::string full_stacktrace;
    std::string brief_stacktrace;
};

static inline void dump_stacks(FILE *log_file,
                               cJSON *json_mem_arr,
                               std::map<uint64_t, stack_meta_t> &stack_metas) {
    if (stack_metas.empty()) {
        LOGI(TAG, "stacktrace: nothing dump");
        return;
    }

    LOGD(TAG, "dump_stacks: hash count = %zu", stack_metas.size());
    flogger(log_file, "dump_stacks: hash count = %zu\n", stack_metas.size());

    std::unordered_map<std::string, size_t> stack_alloc_size_of_so;
    std::unordered_map<std::string, std::vector<stack_dump_meta_t>> stacktrace_of_so;

    for (auto &stack_meta_it : stack_metas) {
        auto hash       = stack_meta_it.first;
        auto size       = stack_meta_it.second.size;
        auto backtrace = stack_meta_it.second.backtrace;
        auto caller     = stack_meta_it.second.caller;

        std::string caller_so_name;
        std::stringstream full_stack_builder;
        std::stringstream brief_stack_builder;


        Dl_info caller_info{};
        dladdr(caller, &caller_info);

        if (caller_info.dli_fname != nullptr) {
            caller_so_name = caller_info.dli_fname;
        }

        std::string last_so_name; // 上一帧所属 so 名字

        auto _callback = [&](wechat_backtrace::FrameDetail it) {
            std::string so_name = it.map_name;

            char *demangled_name = nullptr;
            int status = 0;
            demangled_name = abi::__cxa_demangle(it.function_name, nullptr, 0, &status);

            full_stack_builder << "      | "
                               << "#pc " << std::hex << it.rel_pc << " "
                               << (demangled_name ? demangled_name : "(null)")
                               << " ("
                               << it.map_name
                               << ")"
                               << std::endl;

            if (last_so_name != it.map_name) {
                last_so_name = it.map_name;
                brief_stack_builder << it.map_name << ";";
            }

            brief_stack_builder << std::hex << it.rel_pc << ";";

            if (demangled_name) {
                free(demangled_name);
            }

            if (caller_so_name.empty()) { // fallback
                LOGD(TAG, "fallback getting so name -> caller = %p", stack_meta_it.second.caller);
                // fixme hard coding
                if (/*so_name.find("com.tencent.mm") == std::string::npos ||*/
                        so_name.find("libwechatbacktrace.so") != std::string::npos ||
                        so_name.find("libmatrix-hooks.so") != std::string::npos) {
                    return;
                }
                caller_so_name = so_name;
            }
        };

        wechat_backtrace::restore_frame_detail(backtrace.frames.get(), backtrace.frame_size,
                                               _callback);

//        for (auto   &it : stacktrace) {
//
//            std::string so_name = it.map_name;
//
//            char *demangled_name = nullptr;
//            int  status          = 0;
//            demangled_name = abi::__cxa_demangle(it.function_name.c_str(), nullptr, 0, &status);
//
//            full_stack_builder << "      | "
//                               << "#pc " << std::hex << it.rel_pc << " "
//                               << (demangled_name ? demangled_name : "(null)")
//                               << " ("
//                               << it.map_name
//                               << ")"
//                               << std::endl;
//
//            if (last_so_name != it.map_name) {
//                last_so_name = it.map_name;
//                brief_stack_builder << it.map_name << ";";
//            }
//
//            brief_stack_builder << std::hex << it.rel_pc << ";";
//
//            if (demangled_name) {
//                free(demangled_name);
//            }
//
//            if (caller_so_name.empty()) { // fallback
//                LOGD(TAG, "fallback getting so name -> caller = %p", stack_meta_it.second.caller);
//                // fixme hard coding
//                if (/*so_name.find("com.tencent.mm") == std::string::npos ||*/
//                        so_name.find("libwxperf.so") != std::string::npos ||
//                        so_name.find("libwxperf-jni.so") != std::string::npos) {
//                    continue;
//                }
//                caller_so_name = so_name;
//            }
//        }
        stack_alloc_size_of_so[caller_so_name] += size;

        stack_dump_meta_t stack_dump_meta{size,
                                          full_stack_builder.str(),
                                          brief_stack_builder.str()};
        stacktrace_of_so[caller_so_name].emplace_back(stack_dump_meta);
    }

    // 从大到小排序
    std::vector<std::pair<std::string, size_t>> so_sorted_by_size;
    so_sorted_by_size.reserve(stack_alloc_size_of_so.size());
    for (auto &p : stack_alloc_size_of_so) {
        so_sorted_by_size.emplace_back(p);
    }
    std::sort(so_sorted_by_size.begin(), so_sorted_by_size.end(),
              [](const std::pair<std::string, size_t> &v1,
                 const std::pair<std::string, size_t> &v2) {
                  return v1.second > v2.second;
              });

    /*************************** prepared done ********************************/

    size_t json_so_count = 3;

    for (auto &p : so_sorted_by_size) {
        auto so_name = p.first;
        auto so_alloc_size = p.second;

        LOGD(TAG, "\nmalloc size of so (%s) : remaining size = %zu", so_name.c_str(),
             so_alloc_size);
        flogger(log_file, "\nmalloc size of so (%s) : remaining size = %zu\n", so_name.c_str(),
                so_alloc_size);

        if (so_alloc_size < m_stacktrace_log_threshold) {
            flogger(log_file, "skip printing stacktrace for size less than %zu\n",
                    m_stacktrace_log_threshold);
            continue;
        }

        // 从大到小排序
        auto &stacktrace_sorted_by_size = stacktrace_of_so[so_name];
        std::sort(stacktrace_sorted_by_size.begin(), stacktrace_sorted_by_size.end(),
                  [](const stack_dump_meta_t &v1,
                     const stack_dump_meta_t &v2) {
                      return v1.size > v2.size;
                  });

        cJSON *so_obj = nullptr; // nullable
        cJSON *so_stack_arr = nullptr; // nullable
        if (json_so_count) {
            LOGE(TAG
                         ".json", "json_so_count = %zu", json_so_count);
            json_so_count--;
            so_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(so_obj, "so", so_name.c_str());
            cJSON_AddStringToObject(so_obj, "size", std::to_string(so_alloc_size).c_str());
            so_stack_arr = cJSON_AddArrayToObject(so_obj, "top_stacks");
        }

        size_t json_stacktrace_count = 3;

        for (auto &stack_dump_meta : stacktrace_sorted_by_size) {

            LOGD(TAG, "malloc size of the same stack = %zu\n stacktrace : \n%s",
                 stack_dump_meta.size,
                 stack_dump_meta.full_stacktrace.c_str());

            flogger(log_file, "malloc size of the same stack = %zu\n stacktrace : \n%s\n",
                    stack_dump_meta.size,
                    stack_dump_meta.full_stacktrace.c_str());

            if (json_so_count && json_stacktrace_count) {
                json_stacktrace_count--;
                LOGE(TAG
                             ".json", "json_stacktrace_count = %zu", json_stacktrace_count);
                cJSON *stack_obj = cJSON_CreateObject();
                cJSON_AddStringToObject(stack_obj, "size",
                                        std::to_string(stack_dump_meta.size).c_str());
                cJSON_AddStringToObject(stack_obj, "stack",
                                        stack_dump_meta.brief_stacktrace.c_str());
                cJSON_AddItemToArray(so_stack_arr, stack_obj);
            }
        }
        cJSON_AddItemToArray(json_mem_arr, so_obj);
    }
}

static inline void dump_impl(FILE *log_file, FILE *json_file, bool mmap) {

    std::map<void *, caller_meta_t> heap_caller_metas;
    std::map<void *, caller_meta_t> mmap_caller_metas;
    std::map<uint64_t, stack_meta_t> heap_stack_metas;
    std::map<uint64_t, stack_meta_t> mmap_stack_metas;

    size_t ptr_meta_size = collect_metas(heap_caller_metas,
                                         mmap_caller_metas,
                                         heap_stack_metas,
                                         mmap_stack_metas);

    cJSON *json_obj           = cJSON_CreateObject();
    cJSON *so_native_size_arr = cJSON_AddArrayToObject(json_obj, "SoNativeSize");

    // native heap allocation
    dump_callers(log_file, so_native_size_arr, heap_caller_metas);
    cJSON *native_heap_arr = cJSON_AddArrayToObject(json_obj, "NativeHeap");

    dump_stacks(log_file, native_heap_arr, heap_stack_metas);

    if (mmap) {
        // mmap allocation
        LOGD(TAG, "############################# mmap #############################\n\n");
        flogger(log_file,
                "############################# mmap #############################\n\n");

        cJSON *so_mmap_size_arr = cJSON_AddArrayToObject(json_obj, "SoMmapSize");
        dump_callers(log_file, so_mmap_size_arr, mmap_caller_metas);
        cJSON *mmap_arr = cJSON_AddArrayToObject(json_obj, "mmap");
        dump_stacks(log_file, mmap_arr, mmap_stack_metas);
    }

    char *printed = cJSON_PrintUnformatted(json_obj);
    flogger(json_file, "%s", printed);
    LOGD(TAG, "===> %s", printed);
    cJSON_free(printed);
    cJSON_Delete(json_obj);

    flogger(log_file,
            "\n\n---------------------------------------------------\n"
            "<void *, ptr_meta_t> ptr_meta [%zu * %zu = (%zu)]\n"
            "<uint64_t, stack_meta_t> stack_meta [%zu * %zu = (%zu)]\n"
            "---------------------------------------------------\n",

            sizeof(ptr_meta_t) + sizeof(void *), ptr_meta_size,
            (sizeof(ptr_meta_t) + sizeof(void *)) * ptr_meta_size,

            sizeof(stack_meta_t) + sizeof(uint64_t),
            (heap_stack_metas.size() + mmap_stack_metas.size()),
            (sizeof(stack_meta_t) + sizeof(uint64_t)) *
            ((heap_stack_metas.size() + mmap_stack_metas.size())));

    LOGD(TAG,
         "<void *, ptr_meta_t> ptr_meta [%zu * %zu = (%zu)]\n"
         "<uint64_t, stack_meta_t> stack_meta [%zu * %zu = (%zu)]\n",

         sizeof(ptr_meta_t) + sizeof(void *), ptr_meta_size,
         (sizeof(ptr_meta_t) + sizeof(void *)) * ptr_meta_size,

         sizeof(stack_meta_t) + sizeof(uint64_t),
         (heap_stack_metas.size() + mmap_stack_metas.size()),
         (sizeof(stack_meta_t) + sizeof(uint64_t)) *
         ((heap_stack_metas.size() + mmap_stack_metas.size())));
}

void dump(bool enable_mmap, const char *log_path, const char *json_path) {
    LOGD(TAG,
         ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> memory dump begin <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<");


    FILE *log_file  = log_path ? fopen(log_path, "w+") : nullptr;
    FILE *json_file = json_path ? fopen(json_path, "w+") : nullptr;
    LOGD(TAG, "dump path = %s", log_path);

    dump_impl(log_file, json_file, enable_mmap);

    if (log_file) {
        fflush(log_file);
        fclose(log_file);
    }
    if (json_file) {
        fflush(json_file);
        fclose(json_file);
    }

    LOGD(TAG,
         ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> memory dump end <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<");
}

void memory_hook_on_dlopen(const char *file_name, bool *maps_refreshed) {
    LOGD(TAG, "memory_hook_on_dlopen: file %s, h_malloc %p, h_realloc %p, h_free %p", file_name,
         h_malloc, h_realloc, h_free);
    if (is_stacktrace_enabled) {
        if (!*maps_refreshed) {
            wechat_backtrace::notify_maps_changed();
            *maps_refreshed = true;
        }
    }
    srand((unsigned int) time(NULL));
}

