/* Copyright (c) 2015 Martin Ridgers
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef SHELL_PS_H
#define SHELL_PS_H

#include "shell.h"
#include "singleton.h"

#include <Windows.h>

//------------------------------------------------------------------------------
class shell_ps
    : public shell
    , public singleton<shell_ps>
{
public:
                        shell_ps(line_editor* editor);
                        ~shell_ps();
    bool                validate() override;
    bool                initialise() override;
    void                shutdown() override;

private:
    static BOOL WINAPI  read_console(HANDLE input, wchar_t* buffer, DWORD buffer_count, LPDWORD read_in, void* control);
    void                edit_line(wchar_t* buffer, int buffer_count);
    line_editor*        m_line_editor;
};

#endif // SHELL_PS_H