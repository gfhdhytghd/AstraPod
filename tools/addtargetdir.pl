#!/usr/bin/perl
#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#

# addtargetdir.pl - Adds target directory to gcc-generated dependency data

use File::Basename;

my $rbroot = $ARGV[0];
my $builddir = $ARGV[1];
my $depfile = $ARGV[2];
undef $/;

my $target;
my $rootlen = length $rbroot;
my $src;
my %headers;

# Split the input file on any runs of '\' and whitespace.
for (split(/[\s\\]+/m, <STDIN>)) {
    /^(\/)?[^:]+(\:)?$/;
# Save target and continue if this item ends in ':'
    if (!($2 && ($target=$&))) {
        $src = $&;
# If $target is set, prefix it with the target path
        if ($target) {
            my $dir = dirname $src;
	    substr($dir, 0, $rootlen) = $builddir;
            print "\n$dir/$target";
            $target = "";
# Otherwise, check for an incomplete path for the source file
        } elsif (!$1) {
            $src = "$builddir/$src";
        }
        # Ignore generated headers: their rules consume this dependency
        # scan, so making them prerequisites would create a cycle.
        if (defined($depfile) && $src =~ /\.h$/ &&
            index($src, "$rbroot/") == 0 && index($src, "$builddir/") != 0) {
            $headers{$src} = 1;
        }
        print " \\\n  $src";
    }
}
print "\n";
for my $header (sort keys %headers) {
    # A removed header should trigger a rescan of the included makefile.
    # A source which still includes it will fail during compilation.
    print "$depfile: $header\n$header:\n";
}
