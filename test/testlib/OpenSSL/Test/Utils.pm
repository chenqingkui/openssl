package OpenSSL::Test::Utils;

use strict;
use warnings;

use Exporter;
use vars qw($VERSION @ISA @EXPORT @EXPORT_OK %EXPORT_TAGS);
$VERSION = "0.1";
@ISA = qw(Exporter);
@EXPORT = qw(alldisabled anydisabled disabled config available_protocols);

=head1 NAME

OpenSSL::Test::Utils - test utility functions

=head1 SYNOPSIS

  use OpenSSL::Test::Utils;

  my @tls = available_protocols("tls");
  my @dtls = available_protocols("dtls");
  alldisabled("dh", "dsa");
  anydisabled("dh", "dsa");

  config("fips");

=head1 DESCRIPTION

This module provides utility functions for the testing framework.

=cut

use OpenSSL::Test qw/:DEFAULT top_file/;

=over 4

=item B<available_protocols STRING>

Returns a list of strings for all the available SSL/TLS versions if
STRING is "tls", or for all the available DTLS versions if STRING is
"dtls".  Otherwise, it returns the empty list.  The strings in the
returned list can be used with B<alldisabled> and B<anydisabled>.

=item B<alldisabled ARRAY>
=item B<anydisabled ARRAY>

In an array context returns an array with each element set to 1 if the
corresponding feature is disabled and 0 otherwise.

In a scalar context, alldisabled returns 1 if all of the features in
ARRAY are disabled, while anydisabled returns 1 if any of them are
disabled.

=item B<config STRING>

Returns an item from the %config hash in \$TOP/configdata.pm.

=back

=cut

our %available_protocols;
our %disabled;
our %config;
my $configdata_loaded = 0;

sub load_configdata {
    # We eval it so it doesn't run at compile time of this file.
    # The latter would have top_dir() complain that setup() hasn't
    # been run yet.
    my $configdata = top_file("configdata.pm");
    eval { require $configdata;
	   %available_protocols = %configdata::available_protocols;
	   %disabled = %configdata::disabled;
	   %config = %configdata::config;
    };
    $configdata_loaded = 1;
}

# args
#  list of 1s and 0s, coming from check_disabled()
sub anyof {
    my $x = 0;
    foreach (@_) { $x += $_ }
    return $x > 0;
}

# args
#  list of 1s and 0s, coming from check_disabled()
sub allof {
    my $x = 1;
    foreach (@_) { $x *= $_ }
    return $x > 0;
}

# args
#  list of strings, all of them should be names of features
#  that can be disabled.
# returns a list of 1s (if the corresponding feature is disabled)
#  and 0s (if it isn't)
sub check_disabled {
    return map { exists $disabled{lc $_} ? 1 : 0 } @_;
}

# Exported functions #################################################

# args:
#  list of features to check
sub anydisabled {
    load_configdata() unless $configdata_loaded;
    my @ret = check_disabled(@_);
    return @ret if wantarray;
    return anyof(@ret);
}

# args:
#  list of features to check
sub alldisabled {
    load_configdata() unless $configdata_loaded;
    my @ret = check_disabled(@_);
    return @ret if wantarray;
    return allof(@ret);
}

#!!! Kept for backward compatibility
# args:
#  single string
sub disabled {
    anydisabled(@_);
}

sub available_protocols {
    my $protocol_class = shift;
    if (exists $available_protocols{lc $protocol_class}) {
	return @{$available_protocols{lc $protocol_class}}
    }
    return ();
}

sub config {
    return $config{$_[0]};
}

=head1 SEE ALSO

L<OpenSSL::Test>

=head1 AUTHORS

Stephen Henson E<lt>steve@openssl.orgE<gt> and
Richard Levitte E<lt>levitte@openssl.orgE<gt>

=cut

1;