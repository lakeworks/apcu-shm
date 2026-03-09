--TEST--
APC: Windows named shared memory basic store/fetch
--SKIPIF--
<?php
require_once(dirname(__FILE__) . '/skipif.inc');
if (substr(PHP_OS, 0, 3) !== 'WIN') die('skip Windows only');
?>
--INI--
apc.enabled=1
apc.enable_cli=1
apc.shm_name=test_shm_001
apc.shm_size=4M
--FILE--
<?php
// Basic store/fetch with named shared memory enabled
$key = 'win_shm_test_' . getmypid();

// Store and fetch a string
apcu_store($key, 'hello');
var_dump(apcu_fetch($key));

// Store and fetch an array
apcu_store($key . '_arr', [1, 2, 3]);
var_dump(apcu_fetch($key . '_arr'));

// Delete and verify
apcu_delete($key);
var_dump(apcu_exists($key));

// Verify SMA info is accessible
$info = apcu_sma_info(true);
var_dump(isset($info['seg_size']));
var_dump($info['seg_size'] > 0);

// Clean up
apcu_delete($key . '_arr');
?>
===DONE===
--EXPECT--
string(5) "hello"
array(3) {
  [0]=>
  int(1)
  [1]=>
  int(2)
  [2]=>
  int(3)
}
bool(false)
bool(true)
bool(true)
===DONE===
