#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace oal {

class Config {
public:
    bool load_file(const std::string& path, std::string* error = nullptr);

    void apply_command_line(int argc, char** argv);

    bool has(const std::string& key) const;
    void set(const std::string& key, const std::string& value);

    std::string get_string(const std::string& key, const std::string& fallback) const;
    std::int64_t get_int(const std::string& key, std::int64_t fallback) const;
    double get_double(const std::string& key, double fallback) const;
    bool get_bool(const std::string& key, bool fallback) const;

    const std::map<std::string, std::string>& entries() const { return entries_; }
    const std::vector<std::string>& positional() const { return positional_; }

private:
    std::map<std::string, std::string> entries_;
    std::vector<std::string> positional_;
};

std::string trim(const std::string& value);
bool parse_bool(const std::string& value, bool fallback);

class OptionResolver {
public:
    OptionResolver(const Config& config, std::string prefix)
        : config_(config), prefix_(std::move(prefix)) {}

    bool has(const std::string& name) const;
    std::string string(const std::string& name, const std::string& fallback) const;
    std::int64_t integer(const std::string& name, std::int64_t fallback) const;
    double number(const std::string& name, double fallback) const;
    bool boolean(const std::string& name, bool fallback) const;

private:
    std::string key(const std::string& name) const;

    const Config& config_;
    std::string prefix_;
};

}  // namespace oal
