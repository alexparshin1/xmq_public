/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "SinkModule.h"

#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace xmq::exporter {

namespace {
int anchor{};
}

SinkModule::~SinkModule()
    {
        if (handle)
#ifdef _WIN32
            FreeLibrary(static_cast<HMODULE>(handle));
#else
            dlclose(handle);
#endif
    }

bool SinkModule::open(const std::string& name)
    {
        if (!name.ends_with("_sink") || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != name.npos)
            return false;
#ifdef _WIN32
        HMODULE own{};
        wchar_t file[32768];
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&anchor), &own))
            return false;
        const auto length = GetModuleFileNameW(own, file, static_cast<DWORD>(std::size(file)));
        if (!length || length >= std::size(file))
            return false;
        const auto path = std::filesystem::path(file).parent_path() / ("xmq_" + name + ".dll");
        handle = LoadLibraryW(path.c_str());
        const auto describe = handle ? reinterpret_cast<XmqSinkDescribeV1>(GetProcAddress(static_cast<HMODULE>(handle), "xmq_sink_v1")) : nullptr;
        const auto describeExtension = handle ? reinterpret_cast<xmq_extension_describe_fn>(
            GetProcAddress(static_cast<HMODULE>(handle), XMQ_EXTENSION_DESCRIBE_SYMBOL)) : nullptr;
#else
        Dl_info own{};
        if (!dladdr(&anchor, &own) || !own.dli_fname)
            return false;
#ifdef __APPLE__
        constexpr auto suffix = ".dylib";
#else
        constexpr auto suffix = ".so";
#endif
        const auto path = std::filesystem::path(own.dli_fname).parent_path() / ("libxmq_" + name + suffix);
        handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        const auto describe = handle ? reinterpret_cast<XmqSinkDescribeV1>(dlsym(handle, "xmq_sink_v1")) : nullptr;
        const auto describeExtension = handle ? reinterpret_cast<xmq_extension_describe_fn>(
            dlsym(handle, XMQ_EXTENSION_DESCRIBE_SYMBOL)) : nullptr;
#endif
        api = describe ? describe() : nullptr;
        metadata = describeExtension ? describeExtension(XMQ_EXTENSION_ABI_MAJOR, XMQ_EXTENSION_ABI_MINOR) : nullptr;
        return metadata && metadata->name && name == metadata->name && (metadata->capabilities & XMQ_CAP_SINK) &&
               api && api->create && api->destroy && api->deliver;
    }

const char* SinkModule::setting(void* context, const char* key)
{
    if (context == nullptr)
        return "";
    const auto& values = static_cast<SinkModule*>(context)->settings;
    const auto found = values.find(key);
    return found == values.end() ? "" : found->second.c_str();
}

} // namespace xmq::exporter
