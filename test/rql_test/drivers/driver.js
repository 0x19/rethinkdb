// -- load rethinkdb from the proper location
rethinkdbLocation = '';
var path = require('path');
var fs = require('fs');
try {
    rethinkdbLocation = process.env.JAVASCRIPT_DRIVER_DIR;
} catch(e) {
    dirPath = path.resolve(__dirname)
    while (dirPath != path.sep) {
        if (fs.existsSync(path.resolve(dirPath, 'drivers', 'javascript'))) {
            // TODO: try to compile the drivers
            rethinkdbLocation = path.resolve(dirPath, 'build', 'packages', 'js');
            break;
        }
        dirPath = path.dirname(targetPath)
    }
}
if (fs.existsSync(path.resolve(rethinkdbLocation, 'rethinkdb.js')) == false) {
    process.stdout.write('Could not locate the javascript drivers at the expected location: ' + rethinkdbLocation);
    process.exit(1);
}
var r = require(path.resolve(rethinkdbLocation, 'rethinkdb'));
console.log('Using RethinkDB client from: ' + rethinkdbLocation)

// --

var JSPORT = process.argv[2]
var CPPPORT = process.argv[3]

var TRACE_ENABLED = false;

// -- utilities --

failure_count = 0;

function printTestFailure(name, src, messages) {
    failure_count += 1;
    console.log.apply(console,["\nTEST FAILURE: "+name+"\nTEST BODY: "+src+"\n"].concat(messages).concat(["\n"]));
}

function clone(source) {
    result = {}
    for (var key in source) {
        result[key] = source[key];
    }
    return result;
}

function eq_test(one, two) {
    TRACE("eq_test");
    if (one instanceof Function) {
        return one(two);
    } else if (two instanceof Function) {
        return two(one);
    } else if (Array.isArray(one)) {

        if (!Array.isArray(two)) return false;

        if (one.length != two.length) return false;

        // Recurse on each element of array
        for (var i = 0; i < one.length; i++) {
            if (!eq_test(one[i], two[i])) return false;
        }

        return true;

    } else if (one instanceof Object) {

        if (!(two instanceof Object)) return false;

        // TODO: eq_test({},{foo:4}) will return true
        // Recurse on each property of object
        for (var key in one) {
            if(typeof one[key] === 'string'){
                one[key] = one[key].replace(/\nFailed assertion([\r\n]|.)*/m, "");
            }
            if (one.hasOwnProperty(key)) {
                if (!eq_test(one[key], two[key])) return false;
            }
        }
        return true;

    } else {

        // Primitive comparison
        return (typeof one === typeof two) && (one === two)
    }
}

function le_test(a, b){
    if (a instanceof Object && !Array.isArray(a)) {
        if (!(b instanceof Object && !Array.isArray(b))) {
            return false;
        }
        var ka = Object.keys(a).sort();
        var kb = Object.keys(a).sort();
        if (ka < kb) {
            return true;
        }
        if (ka > kb) {
            return false;
        }
        var ret;
        for (k in ka) {
            k = ka[k];
            if (le_test(a[k], b[k])) {
                return true;
            }
            if (le_test(b[k], a[k])) {
                return false;
            }
        }
        return true;
    } else if (b instanceof Object && !Array.isArray(b)) {
        return true;
    } else {
        return a <= b;
    }
}

// -- Curried output test functions --

// Equality comparison
function eq(exp) {
    var fun = function(val) {
        if (!eq_test(val, exp)) {
            return false;
        } else {
            return true;
        }
    };
    fun.toString = function() {
        return exp.toString();
    };
    return fun;
}

// Tests are stored in list until they can be sequentially evaluated
var tests = []

// Any variables defined by the tests are stored here
var defines = {}

function TRACE(){
    if (TRACE_ENABLED) {
        console.log.apply(console, ["TRACE"].concat([].splice.call(arguments, 0)));
    }
}

// Connect first to cpp server
r.connect({port:CPPPORT}, function(cpp_conn_err, cpp_conn) {

       if(cpp_conn_err){
           console.log("Failed to connect to server:", cpp_conn_err);
           process.exit(1);

       }

        // Pull a test off the queue and run it
        function runTest() { try {
            var testPair = tests.shift();
            if (testPair) {
                if (testPair instanceof Function) {
                    TRACE("==== runTest == function");
                    testPair();
                    runTest();
                    return;
                } else {
                    var src = testPair[0]
                    var exp_val = testPair[1]
                    var testName = testPair[2];
                    var runopts = testPair[3];
                    var testopts = testPair[4];
                    TRACE("==== runTest == non-function: " + src)
                    
                    if (!runopts) {
                        runopts = {batchConf: {max_els: 3}}
                    } else {
                        for (var opt in runopts) {
                            runopts[opt] = eval(runopts[opt])
                        }
                        if (!("batchConf" in runopts)) {
                            runopts.batchConf = {max_els: 3}
                        }
                    }
                    if (!testopts) {
                        testopts = {}
                    }
                    
                    // - convert expected value into a function for comparison
                    try {
                        with (defines) {
                            var exp_fun = eval(exp_val);
                        }
                    } catch (err) {
                        // Oops, this shouldn't have happened
                        console.log(testName);
                        console.log(exp_val);
                        throw err;
                    }
                    if (!exp_fun)
                        exp_fun = function() { return true; };

                    if (!(exp_fun instanceof Function))
                        exp_fun = eq(exp_fun);

                    // - build the test
                    var test = null;
                    try {
                        with (defines) {
                            TRACE("before eval");
                            test = eval(src);
                            TRACE("after eval");
                        }
                    } catch(bld_err) {
                        TRACE("build error")
                        if (exp_fun.isErr) {
                            if (!exp_fun(bld_err)) {
                                printTestFailure(testName, src,
                                    ["Error eval'ing test src not equal to expected err:\n\tERROR: ", bld_err, "\n\tExpected: ", exp_fun]);
                            }
                        } else {
                            printTestFailure(testName, src, ["Error eval'ing test src:\n\t", bld_err]);
                        }

                        // continue to next test
                        runTest();
                        return;
                    }

                    // Run test first on cpp server
                    if (testopts && testopts['reql-query'] == false) {
                        TRACE("non reql-query result: " + test)
                        if (test instanceof Object && test.each) {
                            process_iterable(test, testopts);
                        } else {
                            afterArray(null, test, testopts);
                        }
                    } else {
                        TRACE("processing query: " + test)
                        try {
                            var clone_runopts = runopts ? clone(runopts) : {};
                            var clone_testopts = testopts ? clone(testopts) : {};
                            with (defines) {
                                test.run(cpp_conn, clone_runopts, function(err, cursor) {run_callback(err, cursor, clone_testopts)} );
                            }
                        
                        } catch(err) {
                            TRACE("querry error - " + err)
                            if (exp_fun.isErr) {
                                if (!exp_fun(err)) {
                                    printTestFailure(testName, src, ["Error running test not equal to expected err:\n\tERROR: ", err, "\n\tEXPECTED: ", exp_val]);
                                }
                            } else {
                                printTestFailure(testName, src, ["Error running test:\n\t", err]);
                            }
    
                            // Continue to next test
                            runTest();
                            return;
                        }
                    }
                    
                    function afterArray(cpp_err, cpp_res) { try {
                        TRACE("afterArray - src:" + src + ", err:" + cpp_err + ", result:" + cpp_res + " exected funciton: " + exp_fun);
                        if (cpp_err) {
                            if (exp_fun.isErr) {
                                if (!exp_fun(cpp_err)) {
                                    printTestFailure(testName, src, ["Error running test on CPP server not equal to expected err:", "\n\tERROR: ", JSON.stringify(cpp_err), "\n\tEXPECTED ", exp_val]);
                                }
                            } else {
                                printTestFailure(testName, src, ["Error running test on CPP server:", "\n\tERROR: ", cpp_err.stack]);
                            }
                        } else if (!exp_fun(cpp_res)) {
                            printTestFailure(testName, src, ["CPP result is not equal to expected result:", "\n\tVALUE: ", JSON.stringify(cpp_res), "\n\tEXPECTED: ", exp_val]);
                        }

                        // Continue to next test. Tests are fully sequential
                        // so you can rely on previous queries results in
                        // subsequent tests.
                        runTest();
                        return;
                    } catch(err) {
                        console.log("stack: " + String(err.stack))
                        unexpectedException("afterArray", testName, err);
                    } }
                     
                    function process_iterable(feed, test_options) {
                        TRACE('process_iterable')
                        var accumulator = [];
                        
                        feed.each(
                            function(err, _row) {
                                TRACE('process_iterable_internal')
                                if (err) {
                                    console.log("stack: " + String(err.stack));
                                    unexpectedException("run_callback", testName, err);
                                } else {
                                    try {
                                        if (test_options && test_options.rowfilter) {
                                            filterFunction = new Function('input', test_options.rowfilter);
                                            accumulator.push(filterFunction(_row));
                                        } else {
                                            accumulator.push(_row);
                                        }
                                    } catch(err) {
                                        console.log("stack: " + String(err.stack));
                                        unexpectedException("run_callback: <<" + test_options.filter + ">>", testName, err);
                                    }
                                }
                            },
                            function () {
                                if (test_options && test_options.arrayfilter) {
                                    arrayFunction = new Function('input', test_options.arrayfilter);
                                    accumulator = arrayFunction(accumulator)
                                }
                                afterArray(null, accumulator, test_options);
                            }
                        );
                    }
                    
                    function run_callback(cpp_err, cpp_res_cursor, test_options) { try {
                        TRACE("run_callback src:" + src + ", err:" + cpp_err + ", result:" + cpp_res_cursor);
                        
                        if (test_options && test_options['variable']) {
                            defines[test_options['variable']] = cpp_res_cursor;
                        }
                        
                        if (cpp_err) {
                            afterArray(cpp_err, null, test_options);
                        } else if (cpp_res_cursor instanceof Object && cpp_res_cursor.toArray) {
                            try {
                                cpp_res_cursor.toArray(function (err, result) { afterArray(err, result, test_options)} );
                            } catch(err) {
                                if (err instanceof r.Error.RqlDriverError) {
                                    // probably a Feed
                                    afterArray(null, null, test_options);
                                } else {
                                    throw err;
                                }
                            }
                        } else {
                            afterArray(null, cpp_res_cursor, test_options);
                        }
                    } catch(err) {
                        console.log("stack: " + String(err.stack))
                        unexpectedException("run_callback", testName, err);
                    } }
                }
            } else {
                // We've hit the end of our test list
                // closing the connection will allow the
                // event loop to quit naturally
                cpp_conn.close();

                if(failure_count != 0){
                    console.log("Failed " + failure_count + " tests");
                    process.exit(1);
                } else {
                    console.log("Passed all tests")
                }
            }
        } catch (err) {
            console.log("stack: " + String(err.stack))
            unexpectedException("runTest", testName, testPair[1], err);
        } }

        // Start the recursion though all the tests
        r.dbCreate('test').run(cpp_conn, runTest);
});

function unexpectedException(){
    console.log("Oops, this shouldn't have happened:");
    console.log.apply(console, arguments);
    process.exit(1);
}

// Invoked by generated code to add test and expected result
// Really constructs list of tests to be sequentially evaluated
function test(testSrc, resSrc, name, runopts, testopts) {
    tests.push([testSrc, resSrc, name, runopts, testopts])
}

// Invoked by generated code to define variables to used within
// subsequent tests
function define(expr) {
    tests.push(function() {
        with (defines) {
            eval("defines."+expr);
        }
    });
}

// Invoked by generated code to support bag comparison on this expected value
function bag(list) {
    var bag = eval(list).sort(le_test);
    var fun = function(other) {
        other = other.sort(le_test);
        return eq_test(bag, other);
    };
    fun.toString = function() {
        return "bag("+list+")";
    };
    return fun;
}

// Invoked by generated code to demonstrate expected error output
function err(err_name, err_msg, err_frames) {
    var err_frames = null; // TODO: test for frames
    var fun = function(other) {
        if (!(other instanceof Error)) return false;
        if (err_name && !(other.name === err_name)) return false;

        // Strip out "offending object" from err message
        other.msg = other.msg.replace(/:\n([\r\n]|.)*/m, ".");
        other.msg = other.msg.replace(/\nFailed assertion([\r\n]|.)*/m, "");

        if (err_msg && !(other.msg === err_msg)) return false;
        if (err_frames && !(eq_test(other.frames, err_frames))) return false;
        return true;
    }
    fun.isErr = true;
    fun.toString = function() {
        return err_name+"(\""+err_msg+"\")";
    };
    return fun;
}

function builtin_err(err_name, err_msg) {
    var fun = function(other) {
        if (!(other.name === err_name)) return false;
        if (!(other.message === err_msg)) return false;
        return true;
    }
    fun.isErr = true;
    fun.toString = function() {
        return err_name+"(\""+err_msg+"\")";
    };
    return fun;
}

function arrlen(length, eq_fun) {
    var fun = function(thing) {
        if (!thing.length || thing.length !== length) return false;
        return !eq_fun || thing.every(eq_fun);
    };
    fun.toString = function() {
        return "arrlen("+length+(eq_fun ? ", "+eq_fun.toString() : '')+")";
    };
    return fun;
}

function uuid() {
    var fun = function(thing) {
        return thing.match && thing.match(/[a-z0-9]{8}-[a-z0-9]{4}-[a-z0-9]{4}-[a-z0-9]{4}-[a-z0-9]{12}/);
    };
    fun.toString = function() {
        return "uuid()";
    };
    return fun;
}

function shard() {
    // Don't do anything in JS
}

function the_end(){ }

True = true;
False = false;
