meson setup --prefix=E:/gstreamer/out --vsenv --wipe builddir
meson compile -C .\builddir\
meson install -C .\builddir\

#TOSY disable fontconfig
# option('fontconfig',
#        description : 'Build with FontConfig support. Passing \'auto\' or \'disabled\' disables fontconfig where it is optional, i.e. on Windows and macOS. Passing \'disabled\' on platforms where fontconfig is required results in error.',
#        type: 'feature',
#       value: 'disabled')