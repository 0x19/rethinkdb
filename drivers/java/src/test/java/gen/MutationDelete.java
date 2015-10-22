// Autogenerated by convert_tests.py and process_polyglot.py.
// Do not edit this file directly.
// The template for this file is located at:
// ../../../../../templates/Test.java
package gen;

import com.rethinkdb.RethinkDB;
import com.rethinkdb.gen.exc.*;
import com.rethinkdb.gen.ast.*;
import com.rethinkdb.ast.ReqlAst;
import com.rethinkdb.model.MapObject;
import com.rethinkdb.model.OptArgs;
import com.rethinkdb.net.Connection;
import com.rethinkdb.net.Cursor;
import junit.framework.TestCase;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertArrayEquals;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import org.junit.*;
import org.junit.rules.ExpectedException;

import java.util.Arrays;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.time.OffsetDateTime;
import java.time.ZoneOffset;
import java.time.Instant;
import java.util.stream.LongStream;
import java.util.stream.Collectors;
import java.util.stream.Stream;
import java.util.concurrent.TimeoutException;
import java.util.regex.Pattern;
import java.util.Collections;
import java.nio.charset.StandardCharsets;

import static gen.TestingCommon.*;
import gen.TestingFramework;

public class MutationDelete {
    Logger logger = LoggerFactory.getLogger(MutationDelete.class);
    public static final RethinkDB r = RethinkDB.r;
    public static final Table tbl = r.db("test").table("tbl");

    Connection<?> conn;
    public String hostname = TestingFramework.getConfig().getHostName();
    public int port = TestingFramework.getConfig().getPort();

    @Before
    public void setUp() throws Exception {
        conn = TestingFramework.createConnection();
        try {
            r.dbCreate("test").run(conn);
            r.db("test").wait_().run(conn);
        }catch (Exception e){}
        try {
            r.db("test").tableCreate("tbl").run(conn);
            r.db("test").table(tbl).wait_().run(conn);
        }catch (Exception e){}
    }

    @After
    public void tearDown() throws Exception {
        System.out.println("Tearing down.");
        if(!conn.isOpen()){
            conn.close();
            conn = TestingFramework.createConnection();
        }
        r.db("test").tableDrop("tbl").run(conn);
        r.dbDrop("test").run(conn);
        r.db("rethinkdb").table("_debug_scratch").delete();
        conn.close(false);
    }

    // Autogenerated tests below

        @Test(timeout=120000)
    public void test() throws Exception {
                
        {
            // mutation/delete.yaml #1
            /* ({'deleted':0,'replaced':0,'unchanged':0,'errors':0,'skipped':0,'inserted':100}) */
            Map expected_ = r.hashMap("deleted", 0L).with("replaced", 0L).with("unchanged", 0L).with("errors", 0L).with("skipped", 0L).with("inserted", 100L);
            /* tbl.insert([{'id':i} for i in xrange(100)]) */
            System.out.println("About to run #1: tbl.insert(LongStream.range(0, 100L).boxed().map(i -> r.hashMap('id', i)).collect(Collectors.toList()))");
            Object obtained = runOrCatch(tbl.insert(LongStream.range(0, 100L).boxed().map(i -> r.hashMap("id", i)).collect(Collectors.toList())),
                                          new OptArgs()
                                          ,conn);
            System.out.println("Finished running #1");
            try {
                assertEquals(expected_, obtained);
            System.out.println("Finished asserting #1");
            } catch (Throwable ae) {
                System.out.println("Whoops, got exception on #1:" + ae.toString());
                if(obtained instanceof Throwable) {
                    ae.addSuppressed((Throwable) obtained);
                }
                throw ae;
            }
        }
        
        {
            // mutation/delete.yaml #2
            /* 100 */
            Long expected_ = 100L;
            /* tbl.count() */
            System.out.println("About to run #2: tbl.count()");
            Object obtained = runOrCatch(tbl.count(),
                                          new OptArgs()
                                          ,conn);
            System.out.println("Finished running #2");
            try {
                assertEquals(expected_, obtained);
            System.out.println("Finished asserting #2");
            } catch (Throwable ae) {
                System.out.println("Whoops, got exception on #2:" + ae.toString());
                if(obtained instanceof Throwable) {
                    ae.addSuppressed((Throwable) obtained);
                }
                throw ae;
            }
        }
        
        {
            // mutation/delete.yaml #3
            /* ({'deleted':1,'replaced':0,'unchanged':0,'errors':0,'skipped':0,'inserted':0}) */
            Map expected_ = r.hashMap("deleted", 1L).with("replaced", 0L).with("unchanged", 0L).with("errors", 0L).with("skipped", 0L).with("inserted", 0L);
            /* tbl.get(12).delete() */
            System.out.println("About to run #3: tbl.get(12L).delete()");
            Object obtained = runOrCatch(tbl.get(12L).delete(),
                                          new OptArgs()
                                          ,conn);
            System.out.println("Finished running #3");
            try {
                assertEquals(expected_, obtained);
            System.out.println("Finished asserting #3");
            } catch (Throwable ae) {
                System.out.println("Whoops, got exception on #3:" + ae.toString());
                if(obtained instanceof Throwable) {
                    ae.addSuppressed((Throwable) obtained);
                }
                throw ae;
            }
        }
        
        {
            // mutation/delete.yaml #4
            /* err('ReqlQueryLogicError', 'Durability option `wrong` unrecognized (options are "hard" and "soft").', [0]) */
            Err expected_ = err("ReqlQueryLogicError", "Durability option `wrong` unrecognized (options are \"hard\" and \"soft\").", r.array(0L));
            /* tbl.skip(50).delete(durability='wrong') */
            System.out.println("About to run #4: tbl.skip(50L).delete().optArg('durability', 'wrong')");
            Object obtained = runOrCatch(tbl.skip(50L).delete().optArg("durability", "wrong"),
                                          new OptArgs()
                                          ,conn);
            System.out.println("Finished running #4");
            try {
                assertEquals(expected_, obtained);
            System.out.println("Finished asserting #4");
            } catch (Throwable ae) {
                System.out.println("Whoops, got exception on #4:" + ae.toString());
                if(obtained instanceof Throwable) {
                    ae.addSuppressed((Throwable) obtained);
                }
                throw ae;
            }
        }
        
        {
            // mutation/delete.yaml #5
            /* ({'deleted':49,'replaced':0,'unchanged':0,'errors':0,'skipped':0,'inserted':0}) */
            Map expected_ = r.hashMap("deleted", 49L).with("replaced", 0L).with("unchanged", 0L).with("errors", 0L).with("skipped", 0L).with("inserted", 0L);
            /* tbl.skip(50).delete(durability='soft') */
            System.out.println("About to run #5: tbl.skip(50L).delete().optArg('durability', 'soft')");
            Object obtained = runOrCatch(tbl.skip(50L).delete().optArg("durability", "soft"),
                                          new OptArgs()
                                          ,conn);
            System.out.println("Finished running #5");
            try {
                assertEquals(expected_, obtained);
            System.out.println("Finished asserting #5");
            } catch (Throwable ae) {
                System.out.println("Whoops, got exception on #5:" + ae.toString());
                if(obtained instanceof Throwable) {
                    ae.addSuppressed((Throwable) obtained);
                }
                throw ae;
            }
        }
        
        {
            // mutation/delete.yaml #6
            /* ({'deleted':50,'replaced':0,'unchanged':0,'errors':0,'skipped':0,'inserted':0}) */
            Map expected_ = r.hashMap("deleted", 50L).with("replaced", 0L).with("unchanged", 0L).with("errors", 0L).with("skipped", 0L).with("inserted", 0L);
            /* tbl.delete(durability='hard') */
            System.out.println("About to run #6: tbl.delete().optArg('durability', 'hard')");
            Object obtained = runOrCatch(tbl.delete().optArg("durability", "hard"),
                                          new OptArgs()
                                          ,conn);
            System.out.println("Finished running #6");
            try {
                assertEquals(expected_, obtained);
            System.out.println("Finished asserting #6");
            } catch (Throwable ae) {
                System.out.println("Whoops, got exception on #6:" + ae.toString());
                if(obtained instanceof Throwable) {
                    ae.addSuppressed((Throwable) obtained);
                }
                throw ae;
            }
        }
        
        {
            // mutation/delete.yaml #7
            /* err('ReqlQueryLogicError', 'Expected type SELECTION but found DATUM:', [0]) */
            Err expected_ = err("ReqlQueryLogicError", "Expected type SELECTION but found DATUM:", r.array(0L));
            /* r.expr([1, 2]).delete() */
            System.out.println("About to run #7: r.expr(r.array(1L, 2L)).delete()");
            Object obtained = runOrCatch(r.expr(r.array(1L, 2L)).delete(),
                                          new OptArgs()
                                          ,conn);
            System.out.println("Finished running #7");
            try {
                assertEquals(expected_, obtained);
            System.out.println("Finished asserting #7");
            } catch (Throwable ae) {
                System.out.println("Whoops, got exception on #7:" + ae.toString());
                if(obtained instanceof Throwable) {
                    ae.addSuppressed((Throwable) obtained);
                }
                throw ae;
            }
        }
    }
}
