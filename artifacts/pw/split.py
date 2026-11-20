orig = open('fb.orig.cpp').read()
new = open('fb.new.cpp').read()

# 0377 is the CPU clock only: orig + the first block of new (up to the wallpaper statics)
marker = '/* this frame\'s own background colour, 0 when it has none: see sweet_fb_dominant() */'
i = new.index(marker)
mid = new[:i] + orig[orig.index('static uint32_t *sweet_fb_wall;'):]
open('fb.mid.cpp','w').write(mid)
print('mid ok')
