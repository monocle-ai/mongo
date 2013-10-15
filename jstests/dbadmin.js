load('jstests/aggregation/extras/utils.js');

t = db.dbadmin;
t.save( { x : 1 } );

res = db._adminCommand( "listDatabases" );
assert( res.databases && res.databases.length > 0 , "listDatabases 1 " + tojson(res) );

now = new Date();
x = db._adminCommand( "ismaster" );
assert( x.ismaster , "ismaster failed: " + tojson( x ) )
assert( x.localTime, "ismaster didn't include time: " + tojson(x))
localTimeSkew = x.localTime - now
if ( localTimeSkew >= 50 ) {
    print( "Warning: localTimeSkew " + localTimeSkew + " > 50ms." )
}
assert.lt( localTimeSkew, 500, "isMaster.localTime" )

before = db.runCommand( "serverStatus" )
print(before.uptimeEstimate);
sleep( 5000 )
after = db.runCommand( "serverStatus" )
print(after.uptimeEstimate);
assert.lt( 2 , after.uptimeEstimate , "up1" )
assert.gt( after.uptimeEstimate , before.uptimeEstimate , "up2" )

// Test startup_log
var stats = db.getSisterDB( "local" ).startup_log.stats();
assert(stats.capped);

var latestStartUpLog = db.getSisterDB( "local" ).startup_log.find().sort( { $natural: -1 } ).limit(1).next();
var serverStatus = db._adminCommand( "serverStatus" );
var cmdLine = db._adminCommand( "getCmdLineOpts" ).parsed;

// Test that the startup log has the expected keys
var verbose = false;
var expectedKeys = ["_id", "hostname", "startTime", "startTimeLocal", "cmdLine", "pid", "buildinfo"];
var keys = Object.keySet(latestStartUpLog);
assert(arrayEq(expectedKeys, keys, verbose), 'startup_log keys failed');

// Tests _id implicitly - should be comprised of host-timestamp
// Setup expected startTime and startTimeLocal from the supplied timestamp
var _id = latestStartUpLog._id.split('-');  // _id should consist of host-timestamp
var _idHost = _id[0];
var _idUptime = _id[1];
var uptimeSinceEpochRounded = Math.floor(_id[1]/1000) * 1000;
var startTime = new Date(uptimeSinceEpochRounded);  // Expected startTime
var startTimeLocal = new Date(latestStartUpLog.startTimeLocal); // Setup startTimeLocal so it can be compared to a local version of startTime
startTimeLocal.setYear(startTime.getFullYear()); // startTimeLocal doesnt have the year
startTimeLocal.setMilliseconds(0); // Remove any milliseconds

assert.eq(_idHost, latestStartUpLog.hostname, "Hostname doesn't match one from _id");
assert.eq(serverStatus.host.split(':')[0], latestStartUpLog.hostname, "Hostname doesn't match one in server status");
assert.eq(startTime, latestStartUpLog.startTime, "StartTime doesn't match one from _id");
assert.eq(startTimeLocal, new Date(latestStartUpLog.startTime.toLocaleString()), "StartTimeLocal doesn't match startTime converted to Local");
assert.eq(cmdLine, latestStartUpLog.cmdLine, "cmdLine doesn't match that from getCmdLineOpts");
assert.eq(serverStatus.pid, latestStartUpLog.pid, "pid doesn't match that from serverStatus");

// Test buildinfo
var buildinfo = db.runCommand( "buildinfo" );
delete buildinfo.ok;  // Delete extra meta info not in startup_log
var isMaster = db._adminCommand( "ismaster" );

// Test buildinfo has the expected keys
var expectedKeys = ["version", "gitVersion", "sysInfo", "loaderFlags", "compilerFlags", "allocator", "versionArray", "javascriptEngine", "bits", "debug", "maxBsonObjectSize"];
var keys = Object.keySet(latestStartUpLog.buildinfo);
assert(arrayEq(expectedKeys, keys, verbose), "buildinfo keys failed");
assert.eq(buildinfo, latestStartUpLog.buildinfo, "buildinfo doesn't match that from buildinfo command");

// Test version and version Array
var version = latestStartUpLog.buildinfo.version.split('-')[0];
var versionArray = latestStartUpLog.buildinfo.versionArray;
var versionArrayCleaned = [];
for(var i in versionArray) if (versionArray[i] > 0) { versionArrayCleaned.push(versionArray[i]); }

assert.eq(serverStatus.version, latestStartUpLog.buildinfo.version, "Mongo version doesn't match that from ServerStatus");
assert.eq(version, versionArrayCleaned.join('.'), "version doesn't match that from the versionArray");
assert(["V8", "SpiderMonkey", "Unknown"].indexOf(latestStartUpLog.buildinfo.javascriptEngine) > -1);
assert.eq(isMaster.maxBsonObjectSize, latestStartUpLog.buildinfo.maxBsonObjectSize, "maxBsonObjectSize doesn't match one from ismaster");
