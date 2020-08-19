#!/usr/bin/env python
# Copyright 2020 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Generates extra flags needed to allow temporarily reverting flag expiry.

This program generates three files:
* A C++ source file, containing definitions of base::Features that unexpire
  flags that expired in recent milestones, along with a definition of a
  definition of a function `flags::ExpiryEnabledForMilestone`
* A C++ header file, containing declarations of those base::Features
* A C++ source fragment, containing definitions of flags_ui::FeatureEntry
  structures for flags corresponding to those base::Features

Which milestones are recent is sourced from //chrome/VERSION in the source tree.
"""

import os
import sys

ROOT_PATH = os.path.join(os.path.dirname(__file__), '..', '..')


def get_chromium_version():
  """Parses the chromium version out of //chrome/VERSION."""
  with open(os.path.join(ROOT_PATH, 'chrome', 'VERSION')) as f:
    for line in f.readlines():
      key, value = line.strip().split('=')
      if key == 'MAJOR':
        return int(value)
  return None


def recent_mstones(mstone):
  """Returns the list of milestones considered 'recent' for the given mstone.

  Flag unexpiry is available only for flags that expired at recent mstones."""
  return [mstone - 1, mstone]


def file_header(prog_name):
  """Returns the header to use on generated files."""
  return """// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This is a generated file. Do not edit it! It was generated by:
//   {prog_name}
""".format(prog_name=prog_name)


def gen_features_impl(prog_name, mstone):
  """Generates the definitions for the unexpiry features and the expiry-check
     function.

  This function generates the contents of a complete C++ source file,
  which defines base::Features for unexpiration of flags from recent milestones,
  as well as a function ExpiryEnabledForMilestone().
  """
  body = file_header(prog_name)
  body += """
#include "base/feature_list.h"
#include "chrome/browser/unexpire_flags_gen.h"

namespace flags {

"""

  features = [(m, 'UnexpireFlagsM' + str(m)) for m in recent_mstones(mstone)]
  for feature in features:
    body += 'const base::Feature k{f} {{\n'.format(f=feature[1])
    body += '  "{f}",\n'.format(f=feature[1])
    body += '  base::FEATURE_DISABLED_BY_DEFAULT\n'
    body += '};\n\n'

  body += """// Returns the unexpire feature for the given mstone, if any.
const base::Feature* GetUnexpireFeatureForMilestone(int milestone) {
  switch (milestone) {
"""

  for feature in features:
    body += '    case {m}: return &k{f};\n'.format(m=feature[0], f=feature[1])
  body += """    default: return nullptr;
  }
}

}  // namespace flags
"""

  return body


def gen_features_header(prog_name, mstone):
  """Generate a header file declaring features and the expiry predicate.

  This header declares the features and function described in
  gen_features_impl().
  """
  body = file_header(prog_name)

  body += """
#ifndef GEN_CHROME_BROWSER_UNEXPIRE_FLAGS_GEN_H_
#define GEN_CHROME_BROWSER_UNEXPIRE_FLAGS_GEN_H_

namespace flags {

"""

  for m in recent_mstones(mstone):
    body += 'extern const base::Feature kUnexpireFlagsM{m};\n'.format(m=m)

  body += """
// Returns the base::Feature used to decide whether flag expiration is enabled
// for a given milestone, if there is such a feature. If not, returns nullptr.
const base::Feature* GetUnexpireFeatureForMilestone(int milestone);

}  // namespace flags

#endif  // GEN_CHROME_BROWSER_UNEXPIRE_FLAGS_GEN_H_
"""

  return body


def gen_flags_fragment(prog_name, mstone):
  """Generates a .inc file containing flag definitions.

  This creates a C++ source fragment defining flags, which are bound to the
  features described in gen_features_impl().
  """
  fragment = """
    {{"temporary-unexpire-flags-m{m}",
     "Temporarily unexpire M{m} flags.",
     "Temporarily unexpire flags that expired as of M{m}. These flags will be"
     " removed soon.",
     kOsAll | flags_ui::kFlagInfrastructure,
     FEATURE_VALUE_TYPE(flags::kUnexpireFlagsM{m})}},
"""

  return '\n'.join([fragment.format(m=m) for m in recent_mstones(mstone)])


def update_file_if_stale(filename, data):
  """Writes data to filename if data is different from file's contents on disk.
  """
  try:
    disk_data = open(filename, 'r').read()
    if disk_data == data:
      return
  except IOError:
    pass
  open(filename, 'w').write(data)


def main():
  mstone = get_chromium_version()

  if not mstone:
    raise ValueError('Can\'t find or understand //chrome/VERSION')

  progname = sys.argv[0]

  # Note the mstone - 1 here: the listed expiration mstone is the last mstone in
  # which that flag is present, not the first mstone in which it is not present.
  update_file_if_stale(sys.argv[1], gen_features_impl(progname, mstone - 1))
  update_file_if_stale(sys.argv[2], gen_features_header(progname, mstone - 1))
  update_file_if_stale(sys.argv[3], gen_flags_fragment(progname, mstone - 1))


if __name__ == '__main__':
  main()