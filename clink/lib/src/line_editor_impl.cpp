// Copyright (c) 2016 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "line_editor_impl.h"
#include "line_buffer.h"
#include "match_pipeline.h"

#include <core/base.h>
#include <core/os.h>
#include <core/path.h>
#include <core/str_iter.h>
#include <core/str_tokeniser.h>
#include <terminal/terminal.h>

//------------------------------------------------------------------------------
line_editor* line_editor_create(const line_editor::desc& desc)
{
    // Check there's at least a terminal.
    if (desc.terminal == nullptr)
        return nullptr;

    return new line_editor_impl(desc);
}

//------------------------------------------------------------------------------
void line_editor_destroy(line_editor* editor)
{
    delete editor;
}



//------------------------------------------------------------------------------
line_editor_impl::line_editor_impl(const desc& desc)
: m_desc(desc)
, m_backend(desc.shell_name)
, m_flags(0)
{
    add_backend(m_backend);
}

//------------------------------------------------------------------------------
void line_editor_impl::initialise()
{
    if (check_flag(flag_init))
        return;

    struct : public editor_backend::binder {
        virtual bool bind(const char* chord, unsigned char key) const override
        {
            return binder->bind(chord, *backend, key);
        }

        ::binder*       binder;
        editor_backend* backend;
    } binder_impl;

    binder_impl.binder = &m_binder;
    for (auto* backend : m_backends)
    {
        binder_impl.backend = backend;
        backend->bind_input(binder_impl);
    }

    set_flag(flag_init);
}

//------------------------------------------------------------------------------
void line_editor_impl::begin_line()
{
    clear_flag(~flag_init);
    set_flag(flag_editing);

    m_bind_resolver.reset();
    m_command_offset = 0;
    m_keys_size = 0;
    m_prev_key = ~0u;

    match_pipeline pipeline(m_matches);
    pipeline.reset();

    m_desc.terminal->begin();
    m_buffer.begin_line();

    line_state line = get_linestate();
    editor_backend::context context = get_context(line);
    for (auto backend : m_backends)
        backend->on_begin_line(m_desc.prompt, context);
}

//------------------------------------------------------------------------------
void line_editor_impl::end_line()
{
    for (auto i = m_backends.rbegin(), n = m_backends.rend(); i != n; ++i)
        i->on_end_line();

    m_buffer.end_line();
    m_desc.terminal->end();

    clear_flag(flag_editing);
}

//------------------------------------------------------------------------------
bool line_editor_impl::add_backend(editor_backend& backend)
{
    editor_backend** slot = m_backends.push_back();
    return (slot != nullptr) ? *slot = &backend, true : false;
}

//------------------------------------------------------------------------------
bool line_editor_impl::add_generator(match_generator& generator)
{
    match_generator** slot = m_generators.push_back();
    return (slot != nullptr) ? *slot = &generator, true : false;
}

//------------------------------------------------------------------------------
bool line_editor_impl::get_line(char* out, int out_size)
{
    if (check_flag(flag_editing))
        end_line();

    if (check_flag(flag_eof))
        return false;

    const char* line = m_buffer.get_buffer();
    str_base(out, out_size).copy(line);
    return true;
}

//------------------------------------------------------------------------------
bool line_editor_impl::edit(char* out, int out_size)
{
    // Update first so the init state goes through.
    while (update())
        m_desc.terminal->select();

    return get_line(out, out_size);
}

//------------------------------------------------------------------------------
bool line_editor_impl::update()
{
    if (!check_flag(flag_init))
        initialise();

    if (!check_flag(flag_editing))
    {
        begin_line();
        update_internal();
        return true;
    }

    int key = m_desc.terminal->read();
    record_input(key);

    if (!m_bind_resolver.is_resolved())
        m_binder.update_resolver(key, m_bind_resolver);

    dispatch();
    m_buffer.draw();

    if (!check_flag(flag_editing))
        return false;

    if (!m_bind_resolver.is_resolved())
        update_internal();

    return true;
}

//------------------------------------------------------------------------------
void line_editor_impl::record_input(unsigned char key)
{
    if (m_keys_size < sizeof_array(m_keys) - 1)
        m_keys[m_keys_size++] = key;
}

//------------------------------------------------------------------------------
void line_editor_impl::dispatch()
{
    if (!m_bind_resolver.is_resolved())
        return;

    m_keys[m_keys_size] = '\0';
    m_keys_size = 0;

    editor_backend* backend = m_bind_resolver.get_backend();
    backend = (backend != nullptr) ? backend : &m_backend;

    editor_backend::result result(editor_backend::result::next);
    if (backend != nullptr)
    {
        int id = m_bind_resolver.get_id();
        line_state line = get_linestate();
        editor_backend::context context = get_context(line);
        result = backend->on_input(m_keys, id, context);
    }

    // MODE4 : magic shifts and masks
    unsigned char value = result.value & 0xff;
    switch (value)
    {
    case editor_backend::result::eof:
        set_flag(flag_eof);
        end_line();
        break;

    case editor_backend::result::done:
        end_line();
        break;

    case editor_backend::result::accept_match:
        accept_match((result.value >> 8) & 0xffff);
        m_bind_resolver.reset();
        break;

    case editor_backend::result::redraw:
        m_buffer.redraw();
        m_bind_resolver.reset();
        break;

    case editor_backend::result::next:
        m_bind_resolver.reset();
        break;

    case editor_backend::result::more_input:
        m_bind_resolver.set_id((result.value >> 8) & 0xff);
        break;
    }
}

//------------------------------------------------------------------------------
void line_editor_impl::find_command_bounds(const char*& start, int& length)
{
    const char* line_buffer = m_buffer.get_buffer();
    unsigned int line_cursor = m_buffer.get_cursor();

    start = line_buffer;
    length = line_cursor;

    if (m_desc.command_delims == nullptr)
        return;

    str_iter token_iter(start, length);
    str_tokeniser tokens(token_iter, m_desc.command_delims);
    tokens.add_quote_pair(m_desc.quote_pair);
    while (tokens.next(start, length));

    // We should expect to reach the cursor. If not then there's a trailing
    // separator and we'll just say the command starts at the cursor.
    if (start + length != line_buffer + line_cursor)
    {
        start = line_buffer + line_cursor;
        length = 0;
    }
}

//------------------------------------------------------------------------------
void line_editor_impl::collect_words()
{
    m_words.clear();

    const char* line_buffer = m_buffer.get_buffer();
    unsigned int line_cursor = m_buffer.get_cursor();

    const char* command_start;
    int command_length;
    find_command_bounds(command_start, command_length);

    m_command_offset = int(command_start - line_buffer);

    str_iter token_iter(command_start, command_length);
    str_tokeniser tokens(token_iter, m_desc.word_delims);
    tokens.add_quote_pair(m_desc.quote_pair);
    while (1)
    {
        int length = 0;
        const char* start = nullptr;
        str_token token = tokens.next(start, length);
        if (!token)
            break;

        // Add the word.
        m_words.push_back();
        *(m_words.back()) = { short(start - line_buffer), length, 0, token.delim };
    }

    // Add an empty word if the cursor is at the beginning of one.
    word* end_word = m_words.back();
    if (!end_word || end_word->offset + end_word->length < line_cursor)
    {
        m_words.push_back();
        *(m_words.back()) = { line_cursor };
    }

    // Adjust for quotes.
    for (word& word : m_words)
    {
        if (word.length == 0)
            continue;

        const char* start = line_buffer + word.offset;

        int start_quoted = (start[0] == m_desc.quote_pair[0]);
        int end_quoted = 0;
        if (word.length > 1)
            end_quoted = (start[word.length - 1] == m_desc.quote_pair[0]);

        word.offset += start_quoted;
        word.length -= start_quoted + end_quoted;
        word.quoted = !!start_quoted;
    }

    // Adjust the completing word for if it's partial.
    end_word = m_words.back();
    int partial = 0;
    for (int j = end_word->length - 1; j >= 0; --j)
    {
        int c = line_buffer[end_word->offset + j];
        if (strchr(m_desc.partial_delims, c) == nullptr)
            continue;

        partial = j + 1;
        break;
    }
    end_word->length = partial;
}

//------------------------------------------------------------------------------
void line_editor_impl::accept_match(unsigned int index)
{
    if (index >= m_matches.get_match_count())
        return;

    const char* match = m_matches.get_match(index);
    if (!*match)
        return;

    word end_word = *(m_words.back());
    int word_start = end_word.offset;
    int word_end = end_word.offset + end_word.length;

    const char* buf_ptr = m_buffer.get_buffer();

    str<288> word;
    word.concat(buf_ptr + word_start, end_word.length);
    word << match;

    // Clean the word if it is a valid file system path.
    if (os::get_path_type(word.c_str()) != os::path_type_invalid)
        path::clean(word);

    m_buffer.remove(word_start, m_buffer.get_cursor());
    m_buffer.set_cursor(word_start);
    m_buffer.insert(word.c_str());

    // If this match doesn't make a new partial word, close it off
    int last_char = int(strlen(match)) - 1;
    if (strchr(m_desc.partial_delims, match[last_char]) == nullptr)
    {
        // Closing quote?
        int pre_offset = end_word.offset - 1;
        if (pre_offset >= 0)
            if (const char* q = strchr(m_desc.quote_pair, buf_ptr[pre_offset]))
                m_buffer.insert(q[1] ? q + 1 : q);

        m_buffer.insert(" ");
    }
}

//------------------------------------------------------------------------------
line_state line_editor_impl::get_linestate() const
{
    return {
        m_buffer.get_buffer(),
        m_buffer.get_cursor(),
        m_command_offset,
        m_words,
    };
}

//------------------------------------------------------------------------------
editor_backend::context line_editor_impl::get_context(const line_state& line) const
{
    line_buffer* buffer = const_cast<rl_buffer*>(&m_buffer);
    return { *m_desc.terminal, *buffer, line, m_matches };
}

//------------------------------------------------------------------------------
void line_editor_impl::set_flag(unsigned char flag)
{
    m_flags |= flag;
}

//------------------------------------------------------------------------------
void line_editor_impl::clear_flag(unsigned char flag)
{
    m_flags &= ~flag;
}

//------------------------------------------------------------------------------
bool line_editor_impl::check_flag(unsigned char flag) const
{
    return ((m_flags & flag) != 0);
}

//------------------------------------------------------------------------------
void line_editor_impl::update_internal()
{
    collect_words();

    const word& end_word = *(m_words.back());

    union key_t {
        struct {
            unsigned int word_offset : 11;
            unsigned int word_length : 10;
            unsigned int cursor_pos  : 11;
        };
        unsigned int value;
    };

    key_t next_key = { end_word.offset, end_word.length };

    key_t prev_key;
    prev_key.value = m_prev_key;
    prev_key.cursor_pos = 0;

    // Should we generate new matches?
    if (next_key.value != prev_key.value)
    {
        line_state line = get_linestate();
        match_pipeline pipeline(m_matches);
        pipeline.reset();
        pipeline.generate(line, m_generators);
        pipeline.fill_info(m_desc.auto_quote_chars);
    }

    next_key.cursor_pos = m_buffer.get_cursor();
    prev_key.value = m_prev_key;

    // Should we sort and select matches?
    if (next_key.value != prev_key.value)
    {
        str<64> needle;
        int needle_start = end_word.offset + end_word.length;
        const char* buf_ptr = m_buffer.get_buffer();
        needle.concat(buf_ptr + needle_start, next_key.cursor_pos - needle_start);

        match_pipeline pipeline(m_matches);
        pipeline.select(needle.c_str());
        pipeline.sort();

        m_prev_key = next_key.value;

        // Tell all the backends that the matches changed.
        line_state line = get_linestate();
        editor_backend::context context = get_context(line);
        for (auto backend : m_backends)
            backend->on_matches_changed(context);
    }
}