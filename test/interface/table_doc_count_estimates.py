#!/usr/bin/env python
# Copyright 2010-2014 RethinkDB, all rights reserved.

"""The `interface.table_doc_count_estimates` test checks that the `doc_count_estimates` field on `r.table(...).info()` behaves as expected."""

import os, pprint, re, sys, time

startTime = time.time()

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), os.path.pardir, 'common')))
import driver, scenario_common, utils, vcoptparse

op = vcoptparse.OptParser()
scenario_common.prepare_option_parser_mode_flags(op)
_, command_prefix, serve_options = scenario_common.parse_mode_flags(op.parse(sys.argv))

r = utils.import_python_driver()

print("Spinning up two servers (%.2fs)" % (time.time() - startTime))
with driver.Cluster(initial_servers=['a', 'b'], output_folder='.', command_prefix=command_prefix, extra_options=serve_options) as cluster:
    cluster.check()
    
    print("Establishing ReQL connection (%.2fs)" % (time.time() - startTime))
    
    conn = r.connect(host=cluster[0].host, port=cluster[0].driver_port)
    
    print("Starting testing (%.2fs)" % (time.time() - startTime))
    
    res = r.db_create("test").run(conn)
    assert res["created"] == 1
    res = r.table_create("test").run(conn)
    assert res["created"] == 1

    res = r.table("test").info().run(conn)
    pprint.pprint(res)
    assert res["doc_count_estimates"] == [0]

    N = 100
    fudge = 2

    res = r.table("test").insert([{"id": "a%d" % i} for i in xrange(N)]).run(conn)
    assert res["inserted"] == N
    res = r.table("test").insert([{"id": "z%d" % i} for i in xrange(N)]).run(conn)
    assert res["inserted"] == N

    res = r.table("test").info().run(conn)
    pprint.pprint(res)
    assert N*2/fudge <= res["doc_count_estimates"][0] <= N*2*fudge

    r.table("test").reconfigure(shards=2, replicas=1).run(conn)
    res = list(r.table_wait("test").run(conn))
    pprint.pprint(res)

    res = r.table("test").info().run(conn)
    pprint.pprint(res)
    assert N/fudge <= res["doc_count_estimates"][0] <= N*fudge
    assert N/fudge <= res["doc_count_estimates"][1] <= N*fudge

    # Make sure that oversharding doesn't break distribution queries
    r.table("test").reconfigure(shards=2, replicas=2).run(conn)
    res = list(r.table_wait("test").run(conn))
    pprint.pprint(res)

    res = r.table("test").info().run(conn)
    pprint.pprint(res)
    assert N/fudge <= res["doc_count_estimates"][0] <= N*fudge
    assert N/fudge <= res["doc_count_estimates"][1] <= N*fudge

    res = r.table("test").filter(r.row["id"].split("").nth(0) == "a").delete().run(conn)
    assert res["deleted"] == N

    res = r.table("test").info().run(conn)
    pprint.pprint(res)
    assert res["doc_count_estimates"][0] <= N/fudge
    assert N/fudge <= res["doc_count_estimates"][1] <= N*fudge

    # Check to make sure that system tables work too
    res = r.db("rethinkdb").table("server_config").info().run(conn)
    pprint.pprint(res)
    assert res["doc_count_estimates"] == [2]

    print("Cleaning up (%.2fs)" % (time.time() - startTime))
print("Done. (%.2fs)" % (time.time() - startTime))
