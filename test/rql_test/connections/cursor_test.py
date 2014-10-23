#!/usr/bin/env python

from __future__ import print_function

import os, random, subprocess, sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), os.path.pardir, os.path.pardir, 'common')))
import utils

r = utils.import_python_driver()

try:
    xrange
except NameError:
    xrange = range

server_build_dir = sys.argv[1]
if len(sys.argv) >= 3:
    lang = sys.argv[2]
else:
    lang = None

res = 0

with Cluster.create_cluster(4, server_build_dir=server_build_dir) as cluster:
    server = cluster.processes[0]
    c = r.connect(host=server.host, port=server.driver_port)
    r.db_create('test').run(c)
    r.db('test').table_create('test').run(c)
    tbl = r.table('test')

    num_rows = random.randint(1111, 2222)

    print("Inserting %d rows" % num_rows)
    range500 = list(range(0, 500))
    documents = [{'id':i, 'nums':range500} for i in xrange(0, num_rows)]
    chunks = (documents[i : i + 100] for i in xrange(0, len(documents), 100))
    for chunk in chunks:
        tbl.insert(chunk).run(c)
        print('.', end=' ')
        sys.stdout.flush()
    print("Done\n")
    
    basedir = os.path.dirname(__file__)
    
    if not lang or lang == 'py':
        print("Running Python")
        res = res | subprocess.call([os.environ.get('INTERPRETER_PATH', 'python'), os.path.join(basedir, "cursor.py"), str(port), str(num_rows)])
        print('')
    if not lang or lang == 'js':
        print("Running JS")
        res = res | subprocess.call(["node", os.path.join(basedir, "cursor.js"), str(port), str(num_rows)])
        print('')
    if not lang or lang == 'js-promise':
        print("Running JS Promise")
        res = res | subprocess.call(["node", os.path.join(basedir, "promise.js"), str(port), str(num_rows)])
        print('')


if res is not 0:
    sys.exit(1)
