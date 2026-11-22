# The CardputerZero package: cpack -G DEB in linux/build/cp0 gives
# scheff-zello-cardputer-zero-v<version>_arm64.deb, laid out for the
# launcher (APPLaunch). Its watcher sees the .desktop come and go, so no
# maintainer scripts.
install(TARGETS scheff-zello RUNTIME DESTINATION share/APPLaunch/bin)
install(FILES packaging/scheff-zello.desktop DESTINATION share/APPLaunch/applications)
install(FILES packaging/icon.png DESTINATION share/APPLaunch/share/images RENAME scheff-zello.png)
# The desktop menu entry and its icon. The store allows no files in
# share/applications or share/pixmaps, so they sit here and the app copies the
# entry into ~/.local/share/applications at each start.
install(FILES packaging/scheff-zello-desktop.desktop DESTINATION share/scheff-zello RENAME scheff-zello.desktop)
install(FILES packaging/icon.png DESTINATION share/scheff-zello RENAME scheff-zello.png)
install(FILES ../LICENSE DESTINATION share/doc/scheff-zello RENAME copyright)

set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME scheff-zello)
set(CPACK_PACKAGE_VERSION "${SCHEFF_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "scheff-zello-cardputer-zero-v${SCHEFF_VERSION}_arm64")
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
# The store matches this email to the publisher's GitHub account.
set(CPACK_PACKAGE_CONTACT "oumike <michael.cojocari@gmail.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Scheff Zello: Zello push-to-talk")
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
  "A Zello push-to-talk client for the M5Stack CardputerZero: hold Space to\ntalk on Zello channels, read and send channel messages, and set it up from\na browser on the same network.")
set(CPACK_DEBIAN_PACKAGE_SECTION APPLaunch)
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE arm64)
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/oumike/scheff-zello")
# Trixie renamed libcurl for the 64-bit time_t move; the older name as a fallback.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libcurl4t64 | libcurl4, libsdl2-2.0-0 (>= 2.26), libopus0, libcjson1, libasound2t64 | libasound2")
set(CPACK_STRIP_FILES ON)
include(CPack)
