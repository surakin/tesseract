#pragma once

// Lookup helpers shared by the *_accessibility.cpp tests (a header rather
// than per-file copies: the unity build merges those files' namespaces).

#include "tk/access_tree.h"

#include <string>

namespace access_test
{

// Depth-first: the first node (including `node`) whose name is `name`.
inline const tk::AccessNode* find_named(const tk::AccessNode& node, const std::string& name)
{
    if (node.name == name)
    {
        return &node;
    }
    for (const auto& ch : node.children)
    {
        if (const tk::AccessNode* f = find_named(ch, name))
        {
            return f;
        }
    }
    return nullptr;
}

// Depth-first: the first node whose name starts with `prefix`.
inline const tk::AccessNode* find_prefix(const tk::AccessNode& node, const std::string& prefix)
{
    if (node.name.rfind(prefix, 0) == 0)
    {
        return &node;
    }
    for (const auto& ch : node.children)
    {
        if (const tk::AccessNode* f = find_prefix(ch, prefix))
        {
            return f;
        }
    }
    return nullptr;
}

} // namespace access_test
