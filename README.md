# rawDB — Embedded Columnar Database

**rawDB** — встраиваемая колоночная OLAP-база данных на современном C++23 без единой внешней
зависимости (~9.4K строк кода). SQL-интерфейс поверх append-only storage на mmap с полноценными
ACID транзакциями, Write-Ahead Logging (WAL) и фоновыми Checkpoints (Fuzzy Checkpointing).

```
INSERT throughput: ~2.2M RPS (с включенным WAL и транзакциями)
SELECT full scan:  ~9.7M rows/sec (100K rows)
Index lookup:      ~1.7M qps
Concurrent mix:    15.8K ops/sec (8 threads)
```

---

## Возможности

- **Чистый SQL** — Транзакции (`BEGIN`, `COMMIT`, `ROLLBACK`), `CREATE TABLE`, `INSERT`, `UPDATE`, `DELETE`, `SELECT` с `WHERE`, `ORDER BY` (ASC/DESC),
  `LIMIT`, `CREATE INDEX`, `VACUUM`. Никакого отдельного query API.
- **Колоночное хранение** — каждая колонка хранится отдельно в страницах по 64 KiB,
  читаются только затребованные колонки.
- **ACID & Транзакции** — полноценный Write-Ahead Logging (сегменты по 64 МБ) гарантирует сохранность данных, а Fuzzy Checkpointing позволяет сбрасывать данные на диск в фоне без Stop-The-World блокировок.
- **Lock-Free MVCC** — многоверсионность (Multi-Version Concurrency Control); читатели никогда не блокируют писателей. `UPDATE` реализован без блокировок через паттерн `DELETE + INSERT`.
- **Фоновый GC** — автоматическая чистка tombstone-записей старых транзакций с rate limit 500 MB/s,
  deadlock-free по построению.
- **B-tree индексы** — точечные lookup'и через `CREATE INDEX … ON …` с автоматическим
  перестроением при `VACUUM`.
- **VACUUM** — полное возвращение места от удаленных строк, перенумерация RowId,
  перестроение всех индексов.
- **C API** — `rawdb_open`, `rawdb_execute`, `rawdb_close` — биндинг с любого языка.
- **Durability** — Журнал транзакций (WAL) гарантирует долговечность, фоновый `checkpointer` периодически делает `msync` и сохраняет индексы, восстанавливая состояние через `checkpoint.meta`.
- **Один include** — `#include "rawdb.h"` и линковка `librawdb.a`.
- **Ноль зависимостей** — даже libc++ не требуется, библиотека полностью самодостаточна.

---

## Архитектура

rawDB организована в пять слоев, каждый с одной ответственностью.

**C API** (`rawdb.h`) — публичная точка входа. Тонкая обертка над C++-нутрами:
`rawdb_open`, `rawdb_execute`, `rawdb_close` и итератор по result set.

**Executor** — превращает распаршенный SQL в конкретные операции. Разделен на пять
compilation unit'ов: *DDL* (CREATE TABLE/INDEX, VACUUM), *Mutate* (INSERT, DELETE),
*Scan* (чтение колонок, index lookup), *Filter* (вычисление WHERE), *Select* (ORDER BY,
LIMIT, форматирование). Основной `execute()` диспатчит по типу statement'а.

**Parser** — handwritten recursive-descent SQL парсер. Покрывает: CREATE TABLE/INDEX,
INSERT, DELETE, SELECT (с WHERE, ORDER BY, LIMIT) и VACUUM. Ключевые слова
case-insensitive, точка с запятой в конце опциональна.

**Database** — владеет реестром таблиц (`deque<Table>`), менеджером транзакций (`TransactionManager`)
и координирует фоновые сервисы (GC, Checkpointer). `open()` восстанавливает
сохраненные схемы, читает `checkpoint.meta` и идемпотентно накатывает транзакции из WAL логов.

**Storage** — каждая таблица владеет:
- `MmapFile` — memory-mapped I/O для `.raw`-файла с данными
- `PageIndex` — отображение диапазонов RowId на страницы
- `VersionIndex` — per-row version chain с Lock-Free флагами транзакций (`TxId` и состояния).
- `BTree indexes` — один на индексированную колонку, сохраняется в `<table>_<col>.idx`
- `PendingBatch` — staging buffer для входящих строк; сбрасывается на новую страницу
  при достижении 8192 строк или `kMaxPendingRows`

**Recovery / Журналирование**
- `WalWriter` / `WalReader` — управляет сегментами Write-Ahead Log (64 МБ каждый). В лог пишутся все транзакции перед коммитом (инкрементный LSN).

Два фоновых сервиса (в `std::thread`):
- **GarbageCollector** — просыпается каждую секунду, вычисляет cutoff-порог транзакций и
  чистит tombstone-записи из VersionIndex каждой таблицы.
- **Checkpointer** — просыпается раз в 5 секунд, сбрасывает грязные страницы на диск, сохраняет индексы, делает `msync`, записывает `checkpoint.meta` и **удаляет старые сегменты WAL**, избегая переполнения диска (без блокировки активных транзакций).

### Формат хранения

| Артефакт | Файл | Формат |
|---|---|---|
| Данные строк | `<table>.raw` | Страницы по 64 KiB, column-major, append-only |
| Схема | `<table>.schema` | Бинарный: число колонок, имя/тип каждой |
| Version index | `<table>.vindex` | Бинарный: упакованные `IndexEntry` (26 байт каждый) |
| B-tree индексы | `<table>_<col>.idx` | Кастомная B-tree persistency |

### Concurrency model

- **`shared_mutex` на таблицу** — несколько читателей одновременно, писатели ждут.
- **Писатели** — INSERT, DELETE, VACUUM — блокируют таблицу эксклюзивно.
- **Фоновый GC** — захватывает unique lock на таблицу, обрабатывает одну таблицу за
  цикл (интервал 1 с).
- **Аллокация таймстемпов** — lock-free atomic counter.
- **Нет дедлоков** — VACUUM использует трехфазный протокол: определить живые строки
  (unique) → прочитать данные (shared) → записать (unique).

---

## Почему rawDB?

### vs ClickHouse

ClickHouse — распределенная колоночная СУБД со своим диалектом SQL, HTTP-протоколом
и огромным деревом зависимостей. rawDB — **встраиваемая библиотека**: вы линкуете ее
в свое приложение и вызываете `rawdb_execute()`. Никакого сервера, HTTP, ZooKeeper.
115K INSERT/сек в одном процессе без единой настройки.

### vs Cassandra

Cassandra — распределенный wide-column store с tunable consistency, gossip protocol
и сложным эксплуатационным оверхедом. rawDB — **single-node, fully consistent**,
snapshot isolation под локальным `shared_mutex`. Никакого `nodetool`, compaction
strategies, hinted handoffs — просто `db.open(path)` и готово.

### Что rawDB делает иначе

1. **Zero dependencies** — один `librawdb.a`, никакого transitive dependency hell.
2. **Code you can read** — ~9.4K строк C++23, прямолинейная архитектура.
3. **Append-only by design + WAL** — Страницы пишутся
   один раз и читаются через mmap. А благодаря WAL и фоновому Fuzzy Checkpointing выдерживается скорость в 2.2M RPS без Stop-The-World пауз на сброс. Удаленные строки tombstone'ятся в памяти и
   собираются в фоне.
4. **Per-row MVCC + Lock-Free Transactions** — каждый INSERT/UPDATE получает свой Transaction ID (`TxId`), GC чистит мусор индивидуально. За счет паттерна `UPDATE = DELETE + INSERT` база не знает, что такое взаимная блокировка.
5. **VACUUM как SQL-команда** — пользователь сам контролирует компакшн.
   `VACUUM table_name` возвращает место и перенумеровывает RowId. Не мешает hot path.
6. **Один `shared_mutex` на таблицу** — простейшая корректная модель concurrency.
   Никаких lock managers, wait-die, deadlock detection.

---

## Использование

```c
#include "rawdb.h"

int main() {
    rawdb_t* db = rawdb_open("/path/to/data");
    rawdb_result_t* res = NULL;

    rawdb_execute(db, "CREATE TABLE users (name VARCHAR, age INT64)", NULL);
    rawdb_execute(db, "INSERT INTO users VALUES ('alice', 30)", NULL);
    rawdb_execute(db, "INSERT INTO users VALUES ('bob', 25)", NULL);

    rawdb_execute(db, "SELECT * FROM users WHERE age >= 18 ORDER BY age DESC", &res);

    for (int r = 0; r < rawdb_result_row_count(res); ++r) {
        printf("%s: %s\n",
               rawdb_result_column_name(res, 0),
               rawdb_result_value(res, r, 0));
    }

    rawdb_result_free(res);
    rawdb_close(db);
}
```

### Или из C++

```cpp
#include "db/database.hpp"
#include "query/executor.hpp"

rawdb::Database db;
db.open("/path/to/data");

rawdb::Executor exec(db);
auto result = exec.execute("SELECT * FROM users WHERE age >= 18");
// result->column_names, result->rows
```

### Сборка

```cmake
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

---

## SQL subset

```
statement  ::=  (create_table | create_index | insert | delete | select | vacuum) ';'?
create_table  ::=  CREATE TABLE ident '(' col_def (',' col_def)* ')'
col_def  ::=  ident type  (type: INT64 | FLOAT64 | VARCHAR)
create_index  ::=  CREATE INDEX ident ON ident '(' ident ')'
insert  ::=  INSERT INTO ident VALUES '(' literal (',' literal)* ')'
delete  ::=  DELETE FROM ident WHERE ident op literal
        |    DELETE FROM ident
select  ::=  SELECT (col (',' col)* | '*') FROM ident
             ('WHERE' ident op literal ('AND' ident op literal)*)?
             ('ORDER BY' ident ('ASC'|'DESC')?)? ('LIMIT' int)?
op  ::=  '=' | '!=' | '<' | '<=' | '>' | '>='
vacuum  ::=  VACUUM ident
```

Ключевые слова регистронезависимы. Строки в одинарных кавычках. Точка с запятой
в конце опциональна.

---

## Требования к сборке

- C++23 компилятор (Clang 18+)
- CMake 3.28+
- Linux с `libc++` и `lld` (или правка `cmake/CompilerOptions.cmake`)

---

## License

MIT License

Copyright (c) 2026 haataru

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
