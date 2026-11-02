#include "platform/display_backend.h"

namespace disp {

DisplayRegistry& DisplayRegistry::instance() {
    static DisplayRegistry r;
    return r;
}

void DisplayRegistry::add(const char* name, CreateFn fn) { map_[name] = fn; }

DisplayBackend* DisplayRegistry::create(const std::string& name) {
    auto it = map_.find(name);
    if (it == map_.end()) return nullptr;
    return it->second();
}

std::vector<std::string> DisplayRegistry::names() const {
    std::vector<std::string> v;
    for (auto& kv : map_) v.push_back(kv.first);
    return v;
}

}  // namespace disp
