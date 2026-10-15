// Core option definitions and current values (SET_VARIABLES, SET_CORE_OPTIONS v1/v2).
#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Json.h"
#include "libretro.h"

namespace rrm::lr {

class CoreOptions {
public:
    // config.coreOptions: overrides the core's defaults, also for options defined later.
    void setOverrides(const json& overrides);
    void clear();

    void defineLegacy(const retro_variable* vars);
    void defineV1(const retro_core_option_definition* defs);
    void defineV2(const retro_core_options_v2* opts);
    void setVisible(const char* key, bool visible);

    // Value for GET_VARIABLE, valid until the option changes; nullptr if unknown.
    const char* get(const char* key);
    // Runtime change; flags the update for GET_VARIABLE_UPDATE.
    void set(const std::string& key, const std::string& value);
    bool consumeUpdated();
    // Looks up a configured or defined value without marking it read.
    std::string peek(const std::string& key) const;

    json definitions() const;

private:
    struct Option {
        std::string key, desc, info, category;
        std::vector<std::string> values, labels;
        std::string defaultValue, value;
        bool visible = true;
    };
    void add(Option opt);

    mutable std::mutex mutex_;
    std::vector<Option> options_;
    std::unordered_map<std::string, size_t> index_;
    std::unordered_map<std::string, std::string> overrides_;
    bool updated_ = false;
};

// JSON scalar -> libretro option string (bools become "enabled"/"disabled").
std::string optionValueString(const json& v);

}  // namespace rrm::lr
