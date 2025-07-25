--TEST--
swoole_process: write in worker
--SKIPIF--
<?php
require __DIR__ . '/../include/skipif.inc'; ?>
--FILE--
<?php
require __DIR__ . '/../include/bootstrap.php';

use Swoole\Atomic;
use Swoole\Process;
use Swoole\Server;

$counter = new Atomic();
$pm = new ProcessManager();
$pm->parentFunc = function () use ($pm) {
    $pm->kill();
};
$pm->childFunc = function () use ($pm, $counter) {
    $serv = new Server('127.0.0.1', $pm->getFreePort(), SWOOLE_PROCESS);
    $process = new Process(function (Process $process) use ($pm, $serv, $counter) {
        if ($counter->get() != 1) {
            $counter->set(1);
            echo "process start\n";
            for ($i = 0; $i < 1024; $i++) {
                $data = $process->read();
                Assert::same(strlen($data), 8192);
            }
            echo "process end\n";
            $pm->wakeup();
        }
    });
    $serv->set([
        'worker_num' => 1,
        'log_file' => '/dev/null',
    ]);
    $serv->on('WorkerStart', function (Server $serv) use ($process, $pm) {
        usleep(1);
        for ($i = 0; $i < 1024; $i++) {
            Assert::same($process->write(str_repeat('A', 8192)), 8192);
        }
    });
    $serv->on('WorkerStop', function (Server $serv) use ($process) {
        echo "worker end\n";
    });
    $serv->on('Receive', function () {});
    $serv->addProcess($process);
    $serv->start();
};
$pm->childFirst();
$pm->run();
?>
--EXPECT--
process start
process end
worker end
