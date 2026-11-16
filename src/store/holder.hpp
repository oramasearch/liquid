// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>

namespace liquid::store_detail {

// The process that holds a lease (liquid #527). A repository's own leases (its checkpoint, its
// directories, its build-cache root) protect what one open repository handle is using, so they
// carry their holder as the last line of the lease owner:
//
//   holder <host> <pid> <start>
//
// <start> is the process's start time in the platform's own units, so a later process that reuses
// the pid is not mistaken for the holder.

// This process's holder line.
[[nodiscard]] std::string process_holder();

enum class holder_state {
    // The owner names no holder, or a holder on another host, whose liveness this host cannot see.
    unknown,
    alive,
    // A holder on this host whose process has ended, or whose pid now belongs to another process.
    dead,
};

// The state of the holder named in a lease owner.
[[nodiscard]] holder_state lease_holder_state(std::string_view owner);

} // namespace liquid::store_detail
