#include "libretro/CoreOptions.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>

namespace rrm::lr {

namespace {

bool equalsNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (SDL_tolower(static_cast<unsigned char>(a[i])) != SDL_tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

std::string str(const char* s) { return s ? std::string(s) : std::string(); }

}  // namespace

std::string optionValueString(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "enabled" : "disabled";
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    if (v.is_number_float()) {
        double d = v.get<double>();
        if (std::floor(d) == d && std::fabs(d) < 1e15) return std::to_string(static_cast<long long>(d));
        char buf[64];
        std::snprintf(buf, sizeof buf, "%g", d);
        return buf;
    }
    return v.dump();
}

void CoreOptions::setOverrides(const json& overrides) {
    std::lock_guard lock(mutex_);
    overrides_.clear();
    if (!overrides.is_object()) return;
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        if (it->is_null()) continue;
        overrides_[it.key()] = optionValueString(*it);
    }
}

void CoreOptions::clear() {
    std::lock_guard lock(mutex_);
    options_.clear();
    index_.clear();
    overrides_.clear();
    updated_ = false;
}

// Caller holds the lock.
void CoreOptions::add(Option opt) {
    if (opt.key.empty()) return;
    if (opt.defaultValue.empty() && !opt.values.empty()) opt.defaultValue = opt.values.front();

    auto valid = [&](const std::string& v) -> std::string {
        if (opt.values.empty()) return v;
        for (const auto& candidate : opt.values) {
            if (candidate == v) return candidate;
        }
        for (const auto& candidate : opt.values) {
            if (equalsNoCase(candidate, v)) return candidate;
        }
        return {};
    };

    std::string value = opt.defaultValue;
    auto existing = index_.find(opt.key);
    auto ov = overrides_.find(opt.key);
    if (existing != index_.end() && !valid(options_[existing->second].value).empty()) {
        value = valid(options_[existing->second].value);  // redefinition keeps the current value
    } else if (ov != overrides_.end()) {
        std::string v = valid(ov->second);
        if (!v.empty()) {
            value = v;
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[libretro] option %s: \"%s\" is not a valid value, using \"%s\"",
                        opt.key.c_str(), ov->second.c_str(), opt.defaultValue.c_str());
        }
    }
    opt.value = value;

    if (existing != index_.end()) {
        opt.visible = options_[existing->second].visible;
        options_[existing->second] = std::move(opt);
    } else {
        index_[opt.key] = options_.size();
        options_.push_back(std::move(opt));
    }
}

void CoreOptions::defineLegacy(const retro_variable* vars) {
    std::lock_guard lock(mutex_);
    for (; vars && vars->key; ++vars) {
        Option opt;
        opt.key = vars->key;
        // "Description; value1|value2|value3", first value is the default.
        std::string spec = str(vars->value);
        size_t semi = spec.find(';');
        if (semi != std::string::npos) {
            opt.desc = spec.substr(0, semi);
            size_t pos = semi + 1;
            while (pos < spec.size() && spec[pos] == ' ') ++pos;
            std::string list = spec.substr(pos);
            size_t start = 0;
            while (start <= list.size()) {
                size_t bar = list.find('|', start);
                std::string v = list.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
                if (!v.empty()) {
                    opt.values.push_back(v);
                    opt.labels.push_back(v);
                }
                if (bar == std::string::npos) break;
                start = bar + 1;
            }
        } else {
            opt.desc = spec;
        }
        add(std::move(opt));
    }
}

void CoreOptions::defineV1(const retro_core_option_definition* defs) {
    std::lock_guard lock(mutex_);
    for (; defs && defs->key; ++defs) {
        Option opt;
        opt.key = defs->key;
        opt.desc = str(defs->desc);
        opt.info = str(defs->info);
        for (int i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX && defs->values[i].value; ++i) {
            opt.values.push_back(defs->values[i].value);
            opt.labels.push_back(defs->values[i].label ? defs->values[i].label : defs->values[i].value);
        }
        opt.defaultValue = str(defs->default_value);
        add(std::move(opt));
    }
}

void CoreOptions::defineV2(const retro_core_options_v2* opts) {
    if (!opts) return;
    std::lock_guard lock(mutex_);
    for (const retro_core_option_v2_definition* d = opts->definitions; d && d->key; ++d) {
        Option opt;
        opt.key = d->key;
        opt.desc = str(d->desc);
        opt.info = str(d->info);
        opt.category = str(d->category_key);
        for (int i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX && d->values[i].value; ++i) {
            opt.values.push_back(d->values[i].value);
            opt.labels.push_back(d->values[i].label ? d->values[i].label : d->values[i].value);
        }
        opt.defaultValue = str(d->default_value);
        add(std::move(opt));
    }
}

void CoreOptions::setVisible(const char* key, bool visible) {
    if (!key) return;
    std::lock_guard lock(mutex_);
    auto it = index_.find(key);
    if (it != index_.end()) options_[it->second].visible = visible;
}

const char* CoreOptions::get(const char* key) {
    if (!key) return nullptr;
    std::lock_guard lock(mutex_);
    auto it = index_.find(key);
    if (it != index_.end()) return options_[it->second].value.c_str();
    // Undeclared keys can still be answered from the user's configuration.
    auto ov = overrides_.find(key);
    if (ov != overrides_.end()) return ov->second.c_str();
    return nullptr;
}

void CoreOptions::set(const std::string& key, const std::string& value) {
    std::lock_guard lock(mutex_);
    auto it = index_.find(key);
    if (it != index_.end()) {
        if (options_[it->second].value != value) {
            options_[it->second].value = value;
            updated_ = true;
        }
    } else {
        overrides_[key] = value;
        updated_ = true;
    }
}

bool CoreOptions::consumeUpdated() {
    std::lock_guard lock(mutex_);
    bool u = updated_;
    updated_ = false;
    return u;
}

std::string CoreOptions::peek(const std::string& key) const {
    std::lock_guard lock(mutex_);
    auto it = index_.find(key);
    if (it != index_.end()) return options_[it->second].value;
    auto ov = overrides_.find(key);
    return ov != overrides_.end() ? ov->second : std::string();
}

json CoreOptions::definitions() const {
    std::lock_guard lock(mutex_);
    json out = json::object();
    for (const auto& o : options_) {
        json j;
        j["desc"] = o.desc;
        if (!o.info.empty()) j["info"] = o.info;
        if (!o.category.empty()) j["category"] = o.category;
        j["values"] = o.values;
        j["labels"] = o.labels;
        j["default"] = o.defaultValue;
        j["value"] = o.value;
        j["visible"] = o.visible;
        out[o.key] = std::move(j);
    }
    return out;
}

}  // namespace rrm::lr
