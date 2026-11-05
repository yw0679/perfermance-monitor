import os
import signal
import subprocess
import time
from pathlib import Path
import re
from getpass import getpass
import pymysql
import pytest


PROJECT_DIR = Path(__file__).resolve().parent.parent

@pytest.fixture(scope="session")
def db():
    connection = pymysql.connect(
        host="127.0.0.1",
        user="monitor",
        password=getpass("数据库密码："),
        database="monitor_db",
        charset="utf8mb4",
        connect_timeout=5,
        autocommit=True,
    )

    try:
        yield connection
    finally:
        connection.close()

'''
通过 pytest fixture 管理 worker 和数据库连接，
从本轮采集日志提取主机标识和样本编号，
再轮询查询 MySQL，检查主表与 CPU 明细是否存在对应记录，并在结束后清理资源。
'''
def test_worker_sample_is_stored(db, worker):
    process, log_path, interval = worker

    # 从本轮日志中取得一条样本，固定下来，后续始终查询它
    deadline = time.monotonic() + 15
    sample = None
    # 等待 worker 采集到一条样本
    while time.monotonic() < deadline:
        assert process.poll() is None, "worker 提前退出"

        output = log_path.read_text(
            encoding="utf-8", errors="replace"
        )
        # 拿到一条样本
        sample = re.search(
        r"\[collect\] (\S+) (\S+) seq=(\d+)"
        r" cpu=([0-9.eE+-]+)% mem=([0-9.eE+-]+)%",
        output,
)

        if sample:
            break

        time.sleep(0.2)

    assert sample is not None, f"未找到样本编号，检查日志：{log_path}"
    # 解析样本信息
    hostname, ip, sequence, cpu_text, mem_text = sample.groups()

    server_name = f"{hostname}_{ip}"
    sequence = int(sequence)
    expected_cpu = float(cpu_text)
    expected_mem = float(mem_text)

    print(f"\n本轮检查：{server_name}, seq={sequence}")

    # manager 异步写库，允许一定等待时间
    deadline = time.monotonic() + 10
    main_count = cpu_count = 0
    # 轮询查数据库
    while time.monotonic() < deadline:
        with db.cursor() as cursor:
            cursor.execute(
                """
                SELECT COUNT(*) FROM server_performance
                WHERE server_name = %s AND sequence = %s
                """,
                (server_name, sequence),
            )
            main_count = cursor.fetchone()[0]

            cursor.execute(
                """
                SELECT COUNT(*) FROM server_cpu_detail
                WHERE server_name = %s AND sequence = %s
                """,
                (server_name, sequence),
            )
            cpu_count = cursor.fetchone()[0]

        if main_count == 1 and cpu_count == 1:
            with db.cursor() as cursor:
                cursor.execute(
                    """
                    SELECT cpu_percent, mem_used_percent
                    FROM server_performance
                    WHERE server_name = %s AND sequence = %s
                    """,
                    (server_name, sequence),
                )
                row = cursor.fetchone()

                assert row is not None, "字段核对时未找到对应样本"
                actual_cpu, actual_mem = row

                print(f"\nCPU：日志={expected_cpu}，数据库={actual_cpu}")
                print(f"内存：日志={expected_mem}，数据库={actual_mem}")

                assert actual_cpu == pytest.approx(
                    expected_cpu, abs=0.001
                ), f"CPU 不一致：host={server_name}, seq={sequence}"

                assert actual_mem == pytest.approx(
                    expected_mem, abs=0.001
                ), f"内存不一致：host={server_name}, seq={sequence}"

                return

        time.sleep(0.2)

    pytest.fail(
        f"样本入库检查失败：host={server_name}, seq={sequence}, "
        f"主表数量={main_count}, CPU明细数量={cpu_count}，"
        f"日志：{log_path}"
    )

@pytest.fixture(
    params=[1, 3, 0, -1],
    ids=["1s", "3s", "0s", "-1s"],
)
def worker(request):
    interval = request.param
    # 通过sudo启动py,worker继承权限
    assert os.geteuid() == 0, "请使用 sudo 运行这份测试"

    # 每次使用不同日志文件，保留测试证据
    log_path = Path(__file__).parent / f"worker-{time.time_ns()}.log"
    print(f"\nworker 日志：{log_path}")

    with log_path.open("w", encoding="utf-8") as log_file:
        process = subprocess.Popen(
            [
                str(PROJECT_DIR / "build/worker/worker"),
                "127.0.0.1:50051",
                str(interval),
            ],
            cwd=PROJECT_DIR,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )

        try:
            # 把进程和日志路径交给测试函数
            yield process, log_path, interval
        finally:
            # 测试通过或失败，都尝试正常停止 worker
            if process.poll() is None:
                process.send_signal(signal.SIGINT)

                try:
                    process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                    pytest.fail("worker 清理超时，已强制终止，请检查日志")

            assert process.returncode == 0, (
                f"worker 未正常退出，退出码：{process.returncode}"
            )


def test_worker_starts_collecting(worker):
    process, log_path, interval = worker

    # 给初始化和首次采集最多15秒
    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        assert process.poll() is None, (
            f"worker 提前退出，退出码：{process.returncode}"
        )

        output = log_path.read_text(
            encoding="utf-8", errors="replace"
        )

        tc_ready = "attached eBPF TC hooks to" in output
        collecting = "[collect]" in output
        cpu_present = " cpu=" in output
        if  interval > 0:
            interval_accepted = f"Push interval: {interval} s" in output
        else:
            interval_accepted = "Push interval: 1 s" in output

        if tc_ready and collecting and cpu_present and interval_accepted:
            return

        time.sleep(0.2)

    pytest.fail(f"等待采集就绪超时，请检查日志：{log_path}")

def test_worker_rejects_invalid_interval():
    result = subprocess.run(
        [
            str(PROJECT_DIR / "build/worker/worker"),
            "127.0.0.1:50051",
            "abc",
        ],
        cwd=PROJECT_DIR,
        capture_output=True,
        text=True,
        timeout=5,
    )
    remove = subprocess.run(
        "rm -f Path(__file__).parent/worker-*.log",
        shell=True
    )

    print(f"\n退出码：{result.returncode}")
    print(f"标准输出：\n{result.stdout}")
    print(f"错误输出：\n{result.stderr}")

    assert result.returncode > 0, (
        "非法间隔应通过错误处理退出；"
        f"实际退出码为 {result.returncode}"
    )