// Copyright (c) 2016 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "binder.h"
#include "bind_resolver.h"

#include <core/base.h>

//------------------------------------------------------------------------------
template <int SIZE> static bool translate_chord(const char* chord, char (&out)[SIZE])
{
    // '\M-x'           = alt-x
    // '\C-x' or '^x'   = ctrl-x
    // '\e[t'           = ESC [ t (aka CSI t)
    // 'abc'            = abc

    int i = 0;
    for (; i < (SIZE - 1) && *chord; ++i, ++chord)
    {
        if (*chord != '\\' && *chord != '^')
        {
            out[i] = *chord;
            continue;
        }

        if (*chord == '^')
        {
            out[i] = *chord & 0x1f;
            continue;
        }

        ++chord;
        switch (*chord)
        {
        case '\0':
            i = SIZE;
            continue;

        case 'M':
            if (chord[1] != '-')
                return false;

            ++chord;
            out[i] = '\x1b';
            continue;

        case 'C':
            if (chord[1] != '-')
                return false;

            ++chord;
            out[i] = *chord & 0x1f;
            continue;

        // Some escape sequences for convenience.
        case 'e':   out[i] = '\x1b';    break;
        case 't':   out[i] = '\t';      break;
        case 'n':   out[i] = '\n';      break;
        case 'r':   out[i] = '\r';      break;
        case '0':   out[i] = '\0';      break;
        default:    out[i] = *chord;    break;
        }
    }

    out[i] = '\0';
    return true;
}

//------------------------------------------------------------------------------
binder::binder()
: m_root({})
, m_next_node(0)
{
}

//------------------------------------------------------------------------------
bool binder::bind(const char* chord, editor_backend& backend, unsigned char id)
{
    // Validate input
    const char* c = chord;
    while (*c)
        if (*c++ < 0)
            return false;

    // Translate from ASCII representation to actual keys.
    char translated[64];
    if (!translate_chord(chord, translated))
        return false;

    chord = translated;

    // Store the backend pointer
    int index = add_backend(backend);
    if (index < 0)
        return false;

    // Add the chord of keys into the node graph.
    node* parent = get_root();
    for (; *chord && parent != nullptr; ++chord)
        parent = insert_child(parent, *chord);

    if (parent == nullptr || parent->usage)
        return false;

    node new_parent = *parent;
    new_parent.usage = node_use_bound;
    new_parent.backend = index;
    new_parent.id_or_child = id;

    *parent = new_parent;
    return true;
}

//------------------------------------------------------------------------------
void binder::update_resolver(unsigned char key, bind_resolver& resolver)
{
    if (resolver.is_resolved())
        resolver.reset();

    int node_index = resolver.get_node_index();
    node* current = (node_index >= 0) ? get_node(node_index) : get_root();

    if (node* next = find_child(current, key))
    {
        // More tree to follow?
        if (next->usage == node_use_parent)
            resolver.set_node_index(int(next - m_nodes));

        // Key binding found?
        else if (next->usage == node_use_bound)
            resolver.resolve(get_backend(next->backend), next->id_or_child);

        return;
    }

    // Unbound, or something went wrong...
    resolver.resolve(nullptr, -1);
}

//------------------------------------------------------------------------------
binder::node* binder::find_child(node* parent, unsigned char key)
{
    node* child = nullptr;
    if (parent->usage == node_use_parent)
        child = get_node(parent->id_or_child);

    for (; child != nullptr; child = get_node(child->sibling))
        if (child->key == key)
            return child;

    return nullptr;
}

//------------------------------------------------------------------------------
binder::node* binder::insert_child(node* parent, unsigned char key)
{
    node* child = find_child(parent, key);
    return (child != nullptr) ? child : add_child(parent, key);
}

//------------------------------------------------------------------------------
binder::node* binder::add_child(node* parent, unsigned char key)
{
    int index = alloc_node();
    if (index == sentinal)
        return nullptr;

    node new_child = {};
    new_child.key = key;
    new_child.sibling = (parent->usage == node_use_parent) ? parent->id_or_child : sentinal;

    node* child = get_node(index);
    *child = new_child;

    parent->usage = node_use_parent;
    parent->id_or_child = index;

    return child;
}

//------------------------------------------------------------------------------
int binder::add_backend(editor_backend& backend)
{
    for (int i = 0, n = m_backends.size(); i < n; ++i)
        if (*(m_backends[i]) == &backend)
            return i;

    editor_backend** slot = m_backends.push_back();
    return (slot != nullptr) ? *slot = &backend, int(slot - m_backends.front()) : -1;
}

//------------------------------------------------------------------------------
binder::node* binder::get_root()
{
    return &m_root;
}

//------------------------------------------------------------------------------
binder::node* binder::get_node(unsigned int index)
{
    return (index < sentinal) ? m_nodes + index : nullptr;
}

//------------------------------------------------------------------------------
int binder::alloc_node()
{
    return (m_next_node < sizeof_array(m_nodes)) ? m_next_node++ : sentinal;
}

//------------------------------------------------------------------------------
editor_backend* binder::get_backend(unsigned int index) const
{
    auto b = m_backends[index]; return b ? *b : nullptr;
}