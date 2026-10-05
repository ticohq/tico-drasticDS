// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

/// The part of the cores' shared UsbStorage the quick menu calls. The host
/// mounts USB drives itself (source/switch/SwitchStorage) and has no library
/// folder editor, so the menu only ever shows paths as they are.
#pragma once

#include <string>
#include <vector>

namespace UsbStorage {

struct Volume {
    std::string id;
    std::string root;
    std::string label;
};

inline std::vector<Volume> Volumes() {
    return {};
}

inline std::string ToToken(const std::string& path) {
    return path;
}

inline std::string Resolve(const std::string& path) {
    return path;
}

inline std::string DisplayName(const std::string& path) {
    return path;
}

} // namespace UsbStorage
