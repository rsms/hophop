#!/usr/bin/env python3

"""Tool to bundle multiple C/C++ source files, inlining any includes.

Latest version available here: https://github.com/cwoffenden/combiner

Note: there are two types of exclusion options: the '-x' flag, which besides
excluding a file also adds an #error directive in place of the #include, and
the '-k' flag, which keeps the #include and doesn't inline the file. The
intended use cases are: '-x' for files that would normally be #if'd out, so
features that 100% won't be used in the amalgamated file, for which every
occurrence adds the error, and '-k' for headers that we wish to manually
include, such as a project's public API, for which occurrences after the first
are removed.

Todo: the error handling could be better, which currently throws and halts
(which is functional just not very friendly).

Author: Carl Woffenden, Numfum GmbH (this script is released under a CC0 license
or Public Domain, whichever is applicable in your jurisdiction)

————

Creative Commons Legal Code

CC0 1.0 Universal

    CREATIVE COMMONS CORPORATION IS NOT A LAW FIRM AND DOES NOT PROVIDE
    LEGAL SERVICES. DISTRIBUTION OF THIS DOCUMENT DOES NOT CREATE AN
    ATTORNEY-CLIENT RELATIONSHIP. CREATIVE COMMONS PROVIDES THIS
    INFORMATION ON AN "AS-IS" BASIS. CREATIVE COMMONS MAKES NO WARRANTIES
    REGARDING THE USE OF THIS DOCUMENT OR THE INFORMATION OR WORKS
    PROVIDED HEREUNDER, AND DISCLAIMS LIABILITY FOR DAMAGES RESULTING FROM
    THE USE OF THIS DOCUMENT OR THE INFORMATION OR WORKS PROVIDED
    HEREUNDER.

Statement of Purpose

The laws of most jurisdictions throughout the world automatically confer
exclusive Copyright and Related Rights (defined below) upon the creator
and subsequent owner(s) (each and all, an "owner") of an original work of
authorship and/or a database (each, a "Work").

Certain owners wish to permanently relinquish those rights to a Work for
the purpose of contributing to a commons of creative, cultural and
scientific works ("Commons") that the public can reliably and without fear
of later claims of infringement build upon, modify, incorporate in other
works, reuse and redistribute as freely as possible in any form whatsoever
and for any purposes, including without limitation commercial purposes.
These owners may contribute to the Commons to promote the ideal of a free
culture and the further production of creative, cultural and scientific
works, or to gain reputation or greater distribution for their Work in
part through the use and efforts of others.

For these and/or other purposes and motivations, and without any
expectation of additional consideration or compensation, the person
associating CC0 with a Work (the "Affirmer"), to the extent that he or she
is an owner of Copyright and Related Rights in the Work, voluntarily
elects to apply CC0 to the Work and publicly distribute the Work under its
terms, with knowledge of his or her Copyright and Related Rights in the
Work and the meaning and intended legal effect of CC0 on those rights.

1. Copyright and Related Rights. A Work made available under CC0 may be
protected by copyright and related or neighboring rights ("Copyright and
Related Rights"). Copyright and Related Rights include, but are not
limited to, the following:

  i. the right to reproduce, adapt, distribute, perform, display,
     communicate, and translate a Work;
 ii. moral rights retained by the original author(s) and/or performer(s);
iii. publicity and privacy rights pertaining to a person's image or
     likeness depicted in a Work;
 iv. rights protecting against unfair competition in regards to a Work,
     subject to the limitations in paragraph 4(a), below;
  v. rights protecting the extraction, dissemination, use and reuse of data
     in a Work;
 vi. database rights (such as those arising under Directive 96/9/EC of the
     European Parliament and of the Council of 11 March 1996 on the legal
     protection of databases, and under any national implementation
     thereof, including any amended or successor version of such
     directive); and
vii. other similar, equivalent or corresponding rights throughout the
     world based on applicable law or treaty, and any national
     implementations thereof.

2. Waiver. To the greatest extent permitted by, but not in contravention
of, applicable law, Affirmer hereby overtly, fully, permanently,
irrevocably and unconditionally waives, abandons, and surrenders all of
Affirmer's Copyright and Related Rights and associated claims and causes
of action, whether now known or unknown (including existing as well as
future claims and causes of action), in the Work (i) in all territories
worldwide, (ii) for the maximum duration provided by applicable law or
treaty (including future time extensions), (iii) in any current or future
medium and for any number of copies, and (iv) for any purpose whatsoever,
including without limitation commercial, advertising or promotional
purposes (the "Waiver"). Affirmer makes the Waiver for the benefit of each
member of the public at large and to the detriment of Affirmer's heirs and
successors, fully intending that such Waiver shall not be subject to
revocation, rescission, cancellation, termination, or any other legal or
equitable action to disrupt the quiet enjoyment of the Work by the public
as contemplated by Affirmer's express Statement of Purpose.

3. Public License Fallback. Should any part of the Waiver for any reason
be judged legally invalid or ineffective under applicable law, then the
Waiver shall be preserved to the maximum extent permitted taking into
account Affirmer's express Statement of Purpose. In addition, to the
extent the Waiver is so judged Affirmer hereby grants to each affected
person a royalty-free, non transferable, non sublicensable, non exclusive,
irrevocable and unconditional license to exercise Affirmer's Copyright and
Related Rights in the Work (i) in all territories worldwide, (ii) for the
maximum duration provided by applicable law or treaty (including future
time extensions), (iii) in any current or future medium and for any number
of copies, and (iv) for any purpose whatsoever, including without
limitation commercial, advertising or promotional purposes (the
"License"). The License shall be deemed effective as of the date CC0 was
applied by Affirmer to the Work. Should any part of the License for any
reason be judged legally invalid or ineffective under applicable law, such
partial invalidity or ineffectiveness shall not invalidate the remainder
of the License, and in such case Affirmer hereby affirms that he or she
will not (i) exercise any of his or her remaining Copyright and Related
Rights in the Work or (ii) assert any associated claims and causes of
action with respect to the Work, in either case contrary to Affirmer's
express Statement of Purpose.

4. Limitations and Disclaimers.

 a. No trademark or patent rights held by Affirmer are waived, abandoned,
    surrendered, licensed or otherwise affected by this document.
 b. Affirmer offers the Work as-is and makes no representations or
    warranties of any kind concerning the Work, express, implied,
    statutory or otherwise, including without limitation warranties of
    title, merchantability, fitness for a particular purpose, non
    infringement, or the absence of latent or other defects, accuracy, or
    the present or absence of errors, whether or not discoverable, all to
    the greatest extent permissible under applicable law.
 c. Affirmer disclaims responsibility for clearing rights of other persons
    that may apply to the Work or any use thereof, including without
    limitation any person's Copyright and Related Rights in the Work.
    Further, Affirmer disclaims responsibility for obtaining any necessary
    consents, permissions or other rights required for any use of the
    Work.
 d. Affirmer understands and acknowledges that Creative Commons is not a
    party to this document and has no duty or obligation with respect to
    this CC0 or use of the Work.
"""

import argparse
import re
import sys

from pathlib import Path
from typing import Any, List, Optional, Pattern, Set, TextIO

# Set of file roots when searching (equivalent compiler -I paths).
roots: List[Path] = []

# Set of (canonical) file Path objects to exclude from inlining (and not only
# exclude but to add a compiler error directive when they're encountered).
excludes: Set[Path] = set()

# Set of (canonical) file Path objects to keep as include directives.
keeps: Set[Path] = set()

# Whether to keep the #pragma once directives (unlikely, since this will result
# in a warning, but the option is there).
keep_pragma: bool = False

# Whether to insert /*...*/ comments with details about origin
origin_comments: bool = False

# Whether to insert #line directives
source_map: bool = False

# Destination file object (or stdout if no output file was supplied).
destn: TextIO = sys.stdout

# Set of previously inlined includes (and to ignore if reencountering).
found: Set[Path] = set()

# Compiled regex Pattern to handle the following type of file includes:
#
#   #include "file"
#     #include "file"
#   #  include "file"
#   #include   "file"
#   #include "file" // comment
#   #include "file" // comment with quote "
#
# And all combinations of, as well as ignoring the following:
#
#   #include <file>
#   //#include "file"
#   /*#include "file"*/
#
# We don't try to catch errors since the compiler will do this (and the code is
# expected to be valid before processing) and we don't care what follows the
# file (whether it's a valid comment or not, since anything after the quoted
# string is ignored)
#
include_regex: Pattern[str] = re.compile(r'^\s*#\s*include\s*"(.+?)"')

# Compiled regex Pattern to handle "#pragma once" in various formats:
#
#   #pragma once
#     #pragma once
#   #  pragma once
#   #pragma   once
#   #pragma once // comment
#
# Ignoring commented versions, same as include_regex.
#
pragma_regex: Pattern[str] = re.compile(r'^\s*#\s*pragma\s*once\s*')

def test_match_include() -> bool:
    """Simple tests to prove include_regex's cases."""
    if (include_regex.match('#include "file"')   and
        include_regex.match('  #include "file"') and
        include_regex.match('#  include "file"') and
        include_regex.match('#include   "file"') and
        include_regex.match('#include "file" // comment')):
            if (not include_regex.match('#include <file>')   and
                not include_regex.match('//#include "file"') and
                not include_regex.match('/*#include "file"*/')):
                    matched = include_regex.match('#include "file" // "')
                    if (matched and matched.group(1) == 'file'):
                        print('#include match valid')
                        return True
    return False

def test_match_pragma() -> bool:
    """Simple tests to prove pragma_regex's cases."""
    if (pragma_regex.match('#pragma once')   and
        pragma_regex.match('  #pragma once') and
        pragma_regex.match('#  pragma once') and
        pragma_regex.match('#pragma   once') and
        pragma_regex.match('#pragma once // comment')):
            if (not pragma_regex.match('//#pragma once') and
                not pragma_regex.match('/*#pragma once*/')):
                    print('#pragma once match valid')
                    return True
    return False

def resolve_include(file: str, parent: Optional[Path] = None) -> Optional[Path]:
    """Finds a file. First the currently processing file's 'parent' path is searched,
    followed by the list of 'root' paths, returning a valid Path in
    canonical form. If no match is found None is returned.
    """
    if parent:
        joined = parent.joinpath(file).resolve()
    else:
        joined = Path(file)
    if joined.is_file():
        return joined
    for root in roots:
        joined = root.joinpath(file).resolve()
        if joined.is_file():
            return joined
    return None

def resolve_excluded_files(file_list: Optional[List[str]], resolved: Set[Path], parent: Optional[Path] = None) -> None:
    """Helper to resolve lists of files. 'file_list' is passed in from the args
    and each entry resolved to its canonical path (like any include entry,
    either from the list of root paths or the owning file's 'parent', which in
    this case is case is the input file). The results are stored in 'resolved'.
    """
    if file_list:
        for filename in file_list:
            inc_path = resolve_include(filename, parent)
            if inc_path:
                resolved.add(inc_path)
            else:
                error_line(f'Warning: excluded file not found: {filename}')

def write_line(line: str) -> None:
    """Writes 'line' to the open 'destn' (or stdout)."""
    print(line, file=destn)

def write_comment(line: str) -> None:
    if origin_comments:
        print(line, file=destn)

def error_line(line: Any) -> None:
    """Logs 'line' to stderr. This is also used for general notifications that
    we don't want to go to stdout (so the source can be piped)."""
    print(line, file=sys.stderr)

def add_file(file: Path, file_name: Optional[str] = None) -> None:
    """Inline the contents of 'file' (also inlining its includes, etc.).

    Note: text encoding errors are ignored and replaced with ? when reading the
    input files. This isn't ideal, but it's more than likely in the comments
    than code and a) the text editor has probably also failed to read the same
    content, and b) the compiler probably did too.
    """
    if file.is_file():
        if not file_name:
            file_name = file.name
        # print(f'Processing: {file_name}')
        with file.open('r', errors='replace') as opened:
            for line in opened:
                line = line.rstrip('\n')
                match_include = include_regex.match(line)
                if match_include:
                    # We have a quoted include directive so grab the file
                    inc_name = match_include.group(1)
                    resolved = resolve_include(inc_name, file.parent)
                    if resolved:
                        if resolved in excludes:
                            # The file was excluded so error if the compiler uses it
                            write_line(f'#error Using excluded file: {inc_name} (re-amalgamate source to fix)')
                            print(f'Excluding: {inc_name}')
                        else:
                            if resolved not in found:
                                # The file was not previously encountered
                                found.add(resolved)
                                if resolved in keeps:
                                    # But the include was flagged to keep as included
                                    write_comment(f'/**** *NOT* inlining {inc_name} ****/')
                                    write_line(line)
                                    print(f'Not inlining: {inc_name}')
                                else:
                                    # The file was neither excluded nor seen before so inline it
                                    write_comment(f'/**** start inlining {inc_name} ****/')
                                    if source_map:
                                        write_line(f'#line 1 "{inc_name}"')
                                    add_file(resolved, inc_name)
                                    write_comment(f'/**** ended inlining {inc_name} ****/')
                            else:
                                write_comment(f'/**** skipping file: {inc_name} ****/')
                    else:
                        # The include file didn't resolve to a file
                        write_line(f'#error Unable to find: {inc_name}')
                        error_line(f'Error: Unable to find: {inc_name}')
                else:
                    # Skip any 'pragma once' directives, otherwise write the source line
                    if (keep_pragma or not pragma_regex.match(line)):
                        write_line(line)
    else:
        error_line(f'Error: Invalid file: {file}')

# Start here
parser = argparse.ArgumentParser(description='Amalgamate Tool', epilog=f'example: {sys.argv[0]} -r ../my/path -r ../other/path -o out.c in.c')
parser.add_argument('-r', '--root', action='append', type=Path, help='file root search path')
parser.add_argument('-x', '--exclude',  action='append', help='file to completely exclude from inlining')
parser.add_argument('-k', '--keep', action='append', help='file to exclude from inlining but keep the include directive')
parser.add_argument('-p', '--pragma', action='store_true', default=False, help='keep any "#pragma once" directives (removed by default)')
parser.add_argument('-s', '--source-map', action='store_true', default=False, help='Insert #line directives')
parser.add_argument('-c', '--origin-comments', action='store_true', default=False, help='Insert /*...*/ comments with information about source origin')
parser.add_argument('-o', '--output', type=argparse.FileType('w'), help='output file (otherwise stdout)')
parser.add_argument('input', type=Path, help='input file')
args = parser.parse_args()

# Fail early on an invalid input (and store it so we don't recurse)
args.input = args.input.resolve(strict=True)
found.add(args.input)

# Resolve all of the root paths upfront (we'll halt here on invalid roots)
if args.root:
    for path in args.root:
        p = path.resolve(strict=True)
        if p not in roots:
            roots.append(p)

# The remaining params: so resolve the excluded files and #pragma once directive
resolve_excluded_files(args.exclude, excludes, args.input.parent)
resolve_excluded_files(args.keep,    keeps,    args.input.parent)
keep_pragma = args.pragma
origin_comments = args.origin_comments
source_map = args.source_map

# Then recursively process the input file
try:
    if args.output:
        destn = args.output
    add_file(args.input)
finally:
    if destn:
        destn.close()
