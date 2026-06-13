# rawDB — Embedded Columnar Database

**rawDB** — встраиваемая колоночная OLAP-база данных на современном C++23 без единой внешней зависимости (~9.4K строк кода). SQL-интерфейс поверх append-only storage на mmap с полноценными ACID транзакциями, Write-Ahead Logging (WAL), фоновыми Checkpoints (Fuzzy Checkpointing) и встроенным движком аналитических агрегаций.

```
INSERT throughput: ~2.2M RPS (с включенным WAL и транзакциями)
SELECT full scan:  ~9.7M rows/sec (100K rows)
Index lookup:      ~1.7M qps
Concurrent mix:    15.8K ops/sec (8 threads)
```

---

## Возможности

- **Чистый SQL и Аналитика** — Транзакции (`BEGIN`, `COMMIT`, `ROLLBACK`), `CREATE TABLE`, `INSERT`, `UPDATE`, `DELETE`, `SELECT` с `WHERE`, агрегатные функции (`COUNT`, `SUM`, `AVG`, `MIN`, `MAX`), группировка (`GROUP BY`), сортировка и лимиты. Никакого отдельного query API.
- **Колоночное хранение** — каждая колонка хранится отдельно в страницах по 64 KiB, читаются только затребованные колонки (идеально для аналитики).
- **ACID & Транзакции** — полноценный Write-Ahead Logging гарантирует сохранность данных, а Fuzzy Checkpointing позволяет сбрасывать данные на диск в фоне без Stop-The-World блокировок.
- **Lock-Free MVCC** — многоверсионность (Multi-Version Concurrency Control); читатели никогда не блокируют писателей. `UPDATE` реализован без блокировок через паттерн `DELETE + INSERT`.
- **Фоновый GC** — автоматическая чистка tombstone-записей старых транзакций с rate limit 500 MB/s.
- **B-tree индексы** — точечные lookup'и через `CREATE INDEX … ON …` с автоматическим перестроением при `VACUUM`.
- **VACUUM** — полное возвращение места от удаленных строк, перенумерация RowId, перестроение всех индексов.
- **Мультиязычность** — нативный C/C++ API и официальные высокоскоростные биндинги для **Python** (`pybind11`).
- **Ноль зависимостей** — даже libc++ не требуется, библиотека полностью самодостаточна.

---

## Почему rawDB?

### vs ClickHouse
ClickHouse — распределенная колоночная СУБД со своим огромным деревом зависимостей. rawDB — **встраиваемая библиотека**: вы линкуете ее в свое приложение или импортируете как Python-модуль. Никакого сервера, HTTP, ZooKeeper. **2.2M INSERT/сек** в одном процессе прямо "из коробки" без единой настройки.

### vs Cassandra / SQLite
Cassandra — сложный кластер, требующий DevOps. SQLite — компактная и транзакционная БД, но строчная (row-based), что делает аналитику медленной. rawDB объединяет встраиваемость SQLite со скоростью колоночной архитектуры для агрегации миллионов строк за миллисекунды.

---

## Документация

Вся техническая информация вынесена в папку `docs/`. Рекомендуем начать с архитектуры:

1. 🏛️ **[Архитектура и внутреннее устройство](docs/architecture.md)** — Storage, MVCC, WAL, GC, Checkpoints.
2. 📝 **[Диалект SQL](docs/sql_dialect.md)** — Типы данных, DDL, DML, DQL (SELECT, GROUP BY, агрегации).
3. 🐍 **[Интеграция Python](docs/python_api.md)** — Установка, примеры использования из Python-скриптов.
4. 💻 **[Command Line Interface (CLI)](docs/cli.md)** — Интерактивный REPL терминал (rawdb_cli) для управления базой данных без кода.
5. ⚙️ **[Интеграция C / C++](docs/cpp_c_api.md)** — Подключение статической библиотеки и работа через C/C++ API.

---

## Быстрый старт (Python)

```python
import rawdb

db = rawdb.Database()
db.open("/tmp/data")

conn = rawdb.Connection(db)
exec = rawdb.Executor(conn)

conn.begin()
exec.execute("CREATE TABLE users (name VARCHAR, balance FLOAT64)")
exec.execute("INSERT INTO users VALUES ('alice', 1500.5), ('bob', 200)")
conn.commit()

conn.begin()
# Возвращает [{'COUNT(*)': 2, 'SUM(balance)': 1700.5}]
print(exec.execute("SELECT COUNT(*), SUM(balance) FROM users"))
conn.commit()
```

---

## Сборка (CMake)

Требования:
- C++23 компилятор (Clang 18+)
- CMake 3.28+
- Python 3.x (для сборки биндингов)

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
ctest --test-dir build
```

---

## License
MIT License. Copyright (c) 2026 haataru.
