# Интеграция C / C++ API

rawDB — это встраиваемая (embedded) база данных. У неё нет сервера, HTTP-портов или драйверов для подключения по сети. Вы встраиваете её прямо в своё приложение, линкуя статическую библиотеку `librawdb.a` (или `.so` / `.dll`).

## Использование через C API (`rawdb.h`)

Самый простой и универсальный способ интеграции — использовать стабильный C API.

### Подключение
Добавьте `#include "rawdb.h"` и слинкуйте приложение с библиотекой.

### Пример кода

```c
#include <stdio.h>
#include "rawdb.h"

int main() {
    // 1. Открытие БД
    rawdb_t* db = rawdb_open("/path/to/data/dir");
    if (!db) {
        printf("Failed to open database\n");
        return 1;
    }

    // 2. Выполнение запросов (DDL/DML)
    rawdb_execute(db, "CREATE TABLE stats (id INT64, value FLOAT64)", NULL);
    rawdb_execute(db, "INSERT INTO stats VALUES (1, 100.5), (2, 200.0)", NULL);

    // 3. Выборка данных (DQL)
    rawdb_result_t* res = NULL;
    int status = rawdb_execute(db, "SELECT * FROM stats ORDER BY id DESC", &res);
    
    if (status == 0 && res != NULL) {
        int rows = rawdb_result_row_count(res);
        int cols = rawdb_result_column_count(res);
        
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                printf("%s: %s\t", 
                       rawdb_result_column_name(res, c), 
                       rawdb_result_value(res, r, c));
            }
            printf("\n");
        }
        // Не забудьте освободить память результата!
        rawdb_result_free(res);
    }

    // 4. Закрытие БД (корректно завершает фоновые потоки GC и WAL)
    rawdb_close(db);
    return 0;
}
```

---

## Использование через C++ API

Если вы пишете на C++, вы можете использовать внутренние классы напрямую (минуя обертку C API) для максимальной производительности и типобезопасности.

### Основные классы
- `rawdb::Database` — ядро базы данных.
- `rawdb::Connection` — сессия, управляющая транзакциями (`begin()`, `commit()`, `rollback()`).
- `rawdb::Executor` — исполнитель SQL запросов.

### Пример кода

```cpp
#include <iostream>
#include "db/database.hpp"
#include "query/connection.hpp"
#include "query/executor.hpp"

int main() {
    rawdb::Database db;
    if (db.open("/tmp/rawdb_data") != rawdb::Status::kOk) {
        std::cerr << "Open failed" << std::endl;
        return 1;
    }

    // Создаем сессию
    rawdb::Connection conn(db);
    rawdb::Executor exec(conn);

    // Транзакции поддерживаются нативно
    conn.begin();
    exec.execute("CREATE TABLE metrics (name VARCHAR, rps INT64)");
    exec.execute("INSERT INTO metrics VALUES ('api_v1', 15000)");
    conn.commit();

    // Выполнение аналитики
    conn.begin();
    auto result = exec.execute("SELECT name, SUM(rps) FROM metrics GROUP BY name");
    conn.commit();

    if (result) {
        for (const auto& row : result->rows) {
            std::cout << "Service: " << row[0] << ", Total RPS: " << row[1] << std::endl;
        }
    }

    return 0;
}
```
