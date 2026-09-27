#!/usr/bin/perl

use strict;

sub processFile($)
{
    my ($fileName) = @_;

    open(my $file, $fileName);

    my $newFileName = "$fileName.new";
    open(my $newFile, ">$newFileName");

    my @rows = <$file>;
    close($file);

    my $commentRow = 0;
    my $indent;
    for my $row (@rows) {
        if ($row =~ /^\s*\/\*\*/) {
            $commentRow = 1;
        } else {
            if ($commentRow == 1) {
                $row =~ s/^(\s*\*) ([A-Z].*)\.?$/\1 \@brief \2/;
            }
            $row =~ s/^(\s+\* .*[^.{}\n\r])$/\1./;
            $commentRow++;
            if ($row =~ /^\s+\*\//) {
                $commentRow = 0;
            }
        }

        $row =~ s/(\/\/\/< \w.*[^.\n\r])$/\1./;

        print $newFile $row;
    }

    close($newFile);

    rename ($newFileName, $fileName);
}

opendir(my $dir, ".");
my @files = grep { $_ =~ /\.h$/ } readdir($dir);
closedir($dir);

for my $fileName (@files) {
    processFile($fileName);
}
