use strict;
use warnings;
use ExtUtils::MakeMaker 6.46 qw(WriteMakefile);

my (@CCFLAGS, @INC, @LIBS);

push @INC, '-Iinclude', '-Isrc';
if( $^O eq 'MSWin32' ) {
    push @LIBS, '-Lwin32/ -lzlib';
}
else {
    push @CCFLAGS, '-Wdeclaration-after-statement';
    push @LIBS, '-lz';
}

my $inc_files = join(' ', glob 'include/*.h');
my $src_files = join(' ', glob 'src/*.c');

WriteMakefile(
    NAME              => 'Audio::Scan',
    VERSION_FROM      => 'lib/Audio/Scan.pm',
    CONFIGURE_REQUIRES => { 'ExtUtils::MakeMaker' => 6.46 },
    PREREQ_PM         => { 'Test::Warn' => 0 },
    ABSTRACT_FROM     => 'lib/Audio/Scan.pm',
    AUTHOR            => 'Andy Grundman <andy@hybridized.org>',
    CCFLAGS           => "$Config::Config{ccflags} " . join(' ', @CCFLAGS),
    INC               => join(' ', @INC),
    LIBS              => [ join(' ', @LIBS) ],
    depend            => { 'Scan.c' => "$inc_files $src_files" },
    LICENSE           => 'gpl_2',
    META_MERGE        => {
        # Workaround for
        # https://github.com/Perl-Toolchain-Gang/ExtUtils-MakeMaker/issues/208
        license => [
            'bsd',         # include/mpc.h src/ape.c include/pstdint.h
            'freebsd',     # include/pinttypes.h
            'mit',         # include/ape.h src/ape.c
            'open_source', # buffer.c (SPDX id: SSH-OpenSSH)
            'zlib',        # include/md5.h src/md5.c
        ],
        'meta-spec' => {
            version => 2,
            url => 'https://metacpan.org/pod/CPAN::Meta::Spec',
        },
        resources => {
            homepage => 'https://github.com/LMS-Community/Audio-Scan/',
            license => [
              'https://raw.githubusercontent.com/LMS-Community/Audio-Scan/refs/heads/master/COPYING',
              'https://raw.githubusercontent.com/LMS-Community/Audio-Scan/refs/heads/master/COPYING.ape',
              'https://raw.githubusercontent.com/LMS-Community/Audio-Scan/refs/heads/master/COPYING.buffer',
              'https://raw.githubusercontent.com/LMS-Community/Audio-Scan/refs/heads/master/COPYING.mpc',
              'https://raw.githubusercontent.com/LMS-Community/Audio-Scan/refs/heads/master/COPYING.pinttypes',
              'https://raw.githubusercontent.com/LMS-Community/Audio-Scan/refs/heads/master/COPYING.pstdint',
            ],
            bugtracker => {
                web => 'https://github.com/LMS-Community/Audio-Scan/issues',
            },
            repository => {
                url => 'https://github.com/LMS-Community/Audio-Scan.git',
                web => 'https://github.com/LMS-Community/Audio-Scan/',
                type => 'git',
            },
        },
    },
);
