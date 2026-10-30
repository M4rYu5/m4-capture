The unmodified `plasma-window-management.xml` and `zkde-screencast-unstable-v1.xml` come from KDE's
[plasma-wayland-protocols](https://invent.kde.org/libraries/plasma-wayland-protocols),
retrieved on 2026-10-26 from its GitHub mirror's master branch.

`xdg-output-unstable-v1.xml` comes from the installed wayland-protocols 1.49
package, copied on 2026-10-30. It retains its upstream MIT license notice.

The KDE XML files retain their upstream copyright and LGPL-2.1-or-later notice.
Builds generate client bindings with wayland-scanner. Generated files stay in
the build directory. These KDE interfaces are implementation details and may
require updates when Plasma changes.
