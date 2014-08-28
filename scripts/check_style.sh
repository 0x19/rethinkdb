#!/bin/sh
# Copyright 2010-2014 RethinkDB, all rights reserved.
DIR=`dirname $0`
SRC_DIR=`dirname "$DIR"`/src
IGNORE='/(linenoise\.cc|cJSON\.(cc|hpp)|thread_stack_pcs\.cc|http_parser\.(cc|hpp)|rdb_protocol/geo/s2/.*|rdb_protocol/geo/karney/.*)$'

find "$SRC_DIR" -name \*.cc -o -name \*.hpp -o -name \*.tcc | grep -Ev $IGNORE | xargs $DIR/cpplint --verbose=2 --root="$SRC_DIR" --extensions=hpp,cc,tcc --filter=-build/c++11,-build/include_what_you_use,-whitespace/forcolon,-whitespace/end_of_line,-whitespace/parens,-whitespace/line_length,+readability/casting,-whitespace/braces,-readability/todo,-legal/copyright,-whitespace/comments,-whitespace/labels,-whitespace/blank_line,-readability/function,-readability/braces,-readability/namespace 2>&1

# TODO(2014-09):

# readability/braces - disabled temporarily until somebody goes
# through and fixes all the problems

# readability/namespace - disabled temporarily, until we add a bunch
# of "// namespace foo" comments.
