#include "testscript.h"

#include "util.h"

namespace dr {

TestScript::TestScript(const std::string& text) {
    for (const std::string& raw : split(text, ';')) {
        std::string c = trim(raw);
        if (c.empty())
            continue;
        size_t colon = c.find(':');
        ScriptCommand cmd;
        cmd.name = to_lower(trim(c.substr(0, colon)));
        if (colon != std::string::npos)
            cmd.arg = trim(c.substr(colon + 1));
        cmds_.push_back(cmd);
    }
}

} // namespace dr
