meson setup --prefix=E:/gstreamer/out `
--default-library=shared -Dauto_features=disabled `
-Dtools=enabled -Dbase=enabled -Dgst-plugins-base:opus=enabled `
-Dbad=enabled -Dgst-plugins-bad:openh264=enabled -Dgst-plugins-bad:ipcpipeline=enabled `
-Dgst-plugins-bad:d3d11=enabled -Dgst-plugins-bad:wasapi2=enabled `
--reconfigure buildcodecwin
