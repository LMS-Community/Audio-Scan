on 'configure' => sub {
    requires 'ExtUtils::MakeMaker' => 6.46 ;
    requires 'ExtUtils::MakeMaker::CPANfile';
};

on 'test' => sub {
    requires 'Test::Warn';
};

on 'develop' => sub {
    requires 'App::pod2gfm';
    requires 'ExtUtils::MakeMaker' => 6.52 ;
};
