--TEST--
APC: Windows cross-process shared memory via named segment
--SKIPIF--
<?php
require_once(dirname(__FILE__) . '/skipif.inc');
if (substr(PHP_OS, 0, 3) !== 'WIN') die('skip Windows only');
?>
--INI--
apc.enabled=1
apc.enable_cli=1
apc.shm_name=test_shm_002
apc.shm_size=4M
--FILE--
<?php
// This test verifies that two PHP processes sharing the same apc.shm_name
// can see each other's cached entries via named shared memory.

$unique = 'cross_proc_' . time();
$php = PHP_BINARY;
$ini = '-d apc.enabled=1 -d apc.enable_cli=1 -d apc.shm_name=test_shm_002 -d apc.shm_size=4M';

// Store a value in the current process
apcu_store($unique, 'from_parent');

// Spawn a child process that reads and writes via the same named segment
$child_script = sprintf(
    '%s %s -r %s',
    escapeshellarg($php),
    $ini,
    escapeshellarg(sprintf(
        '$v = apcu_fetch(%s); echo "child_read=" . var_export($v, true) . "\n"; ' .
        'apcu_store(%s, "from_child"); echo "child_wrote=1\n";',
        var_export($unique, true),
        var_export($unique . '_child', true)
    ))
);

$output = [];
exec($child_script, $output, $rc);

// Show child output
foreach ($output as $line) {
    echo $line . "\n";
}

// Read what child wrote
$child_val = apcu_fetch($unique . '_child');
echo "parent_read=" . var_export($child_val, true) . "\n";

// Clean up
apcu_delete($unique);
apcu_delete($unique . '_child');
?>
===DONE===
--EXPECT--
child_read='from_parent'
child_wrote=1
parent_read='from_child'
===DONE===
