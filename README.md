# rawDB — Embedded Columnar Database

**rawDB** — встраиваемая колоночная OLAP-база данных на современном C++23 без единой внешней
зависимости. SQL-интерфейс поверх append-only storage на mmap с MVCC-изоляцией и фоновой
сборкой мусора.

```
INSERT throughput:  115K rows/sec
SELECT full scan:  1.1M rows/sec (100K rows)
Index lookup:      105K qps
Concurrent mix:    11.3K ops/sec (8 threads)
```

---

## Возможности

- **Чистый SQL** — `CREATE TABLE`, `INSERT`, `DELETE`, `SELECT` с `WHERE`, `ORDER BY` (ASC/DESC),
  `LIMIT`, `CREATE INDEX`, `VACUUM`. Никакого отдельного query API.
- **Колоночное хранение** — каждая колонка хранится отдельно в страницах по 64 KiB,
  читаются только затребованные колонки.
- **MVCC** — поплавочные таймстемпы, snapshot isolation; читатели никогда не блокируют писателей.
- **Фоновый GC** — автоматическая чистка tombstone-записей с rate limit 500 MB/s,
  deadlock-free по построению.
- **B-tree индексы** — точечные lookup'и через `CREATE INDEX … ON …` с автоматическим
  перестроением при `VACUUM`.
- **VACUUM** — полное возвращение места от удаленных строк, перенумерация RowId,
  перестроение всех индексов.
- **C API** — `rawdb_open`, `rawdb_execute`, `rawdb_close` — биндинг с любого языка.
- **Durability** — `msync` после каждой записи страницы; schema, version index и B-tree
  переживают перезапуск.
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

**Database** — владеет реестром таблиц (`deque<Table>`), аллокатором таймстемпов
(lock-free atomic counter), глобальными watermark'ами для snapshot isolation и
координирует фоновые сервисы (GC, flush handler). `open()` восстанавливает
сохраненные схемы и version index'ы; `close()` сбрасывает и сохраняет все.

**Storage** — каждая таблица владеет:
- `MmapFile` — memory-mapped I/O для `.raw`-файла с данными
- `PageIndex` — отображение диапазонов RowId на страницы
- `VersionIndex` — per-row version chain (MVCC-видимость, упакованный in-memory index)
- `BTree indexes` — один на индексированную колонку, сохраняется в `<table>_<col>.idx`
- `PendingBatch` — staging buffer для входящих строк; сбрасывается на новую страницу
  при достижении 8192 строк или `kMaxPendingRows`

Два фоновых сервиса:
- **GarbageCollector** — просыпается каждую секунду, вычисляет cutoff timestamp и
  чистит tombstone-записи из VersionIndex каждой таблицы с rate limit 500 MB/s.
- **FlushHandler** — периодически вызывает `msync` на грязных страницах для durability.

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
2. **Code you can read** — ~5000 строк C++23, прямолинейная архитектура.
3. **Append-only by design** — нет WAL, redo log, crash recovery. Страницы пишутся
   один раз и читаются через mmap. Удаленные строки tombstone'ятся в памяти и
   собираются в фоне.
4. **Per-row MVCC** — каждый INSERT получает свой timestamp, GC чистит индивидуально.
   Никаких fixed-size vector windows или merge-tree уровней.
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
