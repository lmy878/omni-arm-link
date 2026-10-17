#include "oal/config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace oal {

std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

bool parse_bool(const std::string& value, bool fallback) {
    const std::string v = trim(value);
    if (v == "1" || v == "true" || v == "yes" || v == "on") {
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off") {
        return false;
    }
    return fallback;
}

bool Config::load_file(const std::string& path, std::string* error) {
    std::ifstream file(path);
    if (!file) {
        if (error != nullptr) {
            *error = "cannot open config file: " + path;
        }
        return false;
    }

    std::string section;
    std::string line;
    int line_no = 0;
    while (std::getline(file, line)) {
        ++line_no;
        const std::string stripped = trim(line);
        if (stripped.empty() || stripped[0] == '#' || stripped[0] == ';') {
            continue;
        }
        if (stripped.front() == '[' && stripped.back() == ']') {
            section = trim(stripped.substr(1, stripped.size() - 2));
            continue;
        }
        const auto eq = stripped.find('=');
        if (eq == std::string::npos) {
            if (error != nullptr) {
                *error = path + ":" + std::to_string(line_no) + ": missing '='";
            }
            return false;
        }
        std::string key = trim(stripped.substr(0, eq));
        const std::string value = trim(stripped.substr(eq + 1));
        if (!section.empty()) {
            key = section + "." + key;
        }
        set(key, value);
    }
    return true;
}

void Config::apply_command_line(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--", 0) != 0) {
            positional_.push_back(arg);
            continue;
        }
        std::string key = arg.substr(2);
        std::string value;
        bool has_value = false;
        const auto eq = key.find('=');
        if (eq != std::string::npos) {
            value = key.substr(eq + 1);
            key = key.substr(0, eq);
            has_value = true;
        }
        for (char& ch : key) {
            if (ch == '-') {
                ch = '_';
            }
        }
        if (key.rfind("no_", 0) == 0) {
            set(key.substr(3), "false");
            continue;
        }
        if (has_value) {
            set(key, value);
            continue;
        }
        if (i + 1 < argc && argv[i + 1][0] != '-') {
            set(key, argv[++i]);
            continue;
        }
        set(key, "true");
    }
}

bool Config::has(const std::string& key) const { return entries_.count(key) != 0; }

void Config::set(const std::string& key, const std::string& value) { entries_[key] = value; }

std::string Config::get_string(const std::string& key, const std::string& fallback) const {
    const auto it = entries_.find(key);
    return it == entries_.end() ? fallback : it->second;
}

std::int64_t Config::get_int(const std::string& key, std::int64_t fallback) const {
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return fallback;
    }
    try {
        return std::stoll(trim(it->second));
    } catch (...) {
        return fallback;
    }
}

double Config::get_double(const std::string& key, double fallback) const {
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return fallback;
    }
    try {
        return std::stod(trim(it->second));
    } catch (...) {
        return fallback;
    }
}

bool Config::get_bool(const std::string& key, bool fallback) const {
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return fallback;
    }
    return parse_bool(it->second, fallback);
}

std::string OptionResolver::key(const std::string& name) const {
    if (config_.has(name)) {
        return name;
    }
    const std::string prefixed = prefix_ + "." + name;
    if (config_.has(prefixed)) {
        return prefixed;
    }
    return name;
}

bool OptionResolver::has(const std::string& name) const {
    return config_.has(name) || config_.has(prefix_ + "." + name);
}

std::string OptionResolver::string(const std::string& name, const std::string& fallback) const {
    return config_.get_string(key(name), fallback);
}

std::int64_t OptionResolver::integer(const std::string& name, std::int64_t fallback) const {
    return config_.get_int(key(name), fallback);
}

double OptionResolver::number(const std::string& name, double fallback) const {
    return config_.get_double(key(name), fallback);
}

bool OptionResolver::boolean(const std::string& name, bool fallback) const {
    return config_.get_bool(key(name), fallback);
}

}  // namespace oal
