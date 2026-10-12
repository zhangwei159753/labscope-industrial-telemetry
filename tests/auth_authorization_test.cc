#include "auth/Authorization.h"

#include <iostream>
#include <stdexcept>

namespace
{
void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    try
    {
        using labscope::auth::Authorization;
        using labscope::auth::Role;
        require(!Authorization::allows(std::nullopt, Role::Viewer),
                "missing session role must fail closed");
        require(Authorization::allows(Role::Viewer, Role::Viewer),
                "viewer may read viewer-level data");
        require(!Authorization::allows(Role::Viewer, Role::Operator),
                "viewer may not perform operator actions");
        require(Authorization::allows(Role::Operator, Role::Viewer) &&
                    Authorization::allows(Role::Operator, Role::Operator) &&
                    !Authorization::allows(Role::Operator, Role::Admin),
                "operator may read and operate, but not administer");
        require(Authorization::allows(Role::Admin, Role::Viewer) &&
                    Authorization::allows(Role::Admin, Role::Operator) &&
                    Authorization::allows(Role::Admin, Role::Admin),
                "admin may satisfy every lower role requirement");
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
