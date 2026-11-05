from getpass import getpass

import pymysql


connection = pymysql.connect(
    host="127.0.0.1",
    user="monitor",
    password=getpass("数据库密码："),
    database="monitor_db",
    charset="utf8mb4",
    connect_timeout=5,
)

try:
    with connection.cursor() as cursor:
        sql = """
            SELECT server_name, sequence, cpu_percent, mem_used_percent
            FROM server_performance
            WHERE server_name = %s AND sequence = %s
        """

        cursor.execute(
            sql,
            ("ubuntu24_192.168.63.133", 1793191097672),
        )

        rows = cursor.fetchall()
        print("查询结果：", rows)
        print("记录数量：", len(rows))
finally:
    connection.close()