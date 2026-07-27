# linuxdeploy sources every *.sh in $APPDIR/apprun-hooks before launching the
# app. .deb/.rpm/Arch packages declare the Noto Color Emoji font as a runtime
# dependency (see packaging/debian/control, packaging/arch/PKGBUILD), but an
# AppImage isn't installed through a package manager, so that font is bundled
# directly under usr/share/fonts instead and pointed to here.
export FONTCONFIG_FILE="$APPDIR/usr/share/fontconfig/tesseract-fonts.conf"
