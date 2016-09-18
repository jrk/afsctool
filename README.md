Source:  
http://hints.macworld.com/article.php?story=20090902223042255
http://forums.macrumors.com/showthread.php?t=780570  
http://files.me.com/brkirch/ijt4f7

---

Command line utility for identifying HFS+ compressed files and getting the sizes of them. Its output looks something like this:

    $ afsctool -v /usr/local/bin/afsctool
    /usr/local/bin/afsctool:
    File is HFS+ compressed.
    File size (uncompressed data fork; reported size by Mac OS 10.6+ Finder): 85372 bytes / 85 KB (kilobytes) / 83 KiB (kibibytes)
    File size (compressed data fork - decmpfs xattr; reported size by Mac OS 10.0-10.5 Finder): 24417 bytes / 25 KB (kilobytes) / 24 KiB (kibibytes)
    File size (compressed data fork): 24433 bytes / 25 KB (kilobytes) / 24 KiB (kibibytes)
    Compression savings: 71.4%
    Number of extended attributes: 0
    Total size of extended attribute data: 0 bytes
    Appoximate overhead of extended attributes: 536 bytes
    Appoximate total file size (compressed data fork + EA + EA overhead + file overhead): 25376 bytes / 25 KB (kilobytes) / 25 KiB (kibibytes)

I've updated afsctool and added in place HFS+ compression for files and folders, just be warned that you should make a backup before attempting to use it as I haven't had a chance to extensively test it yet.

The in place compression seems not to have any significant issues, but if you are compressing anything important then always include the -k flag just to be safe.
