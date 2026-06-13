# Использование через Python (pybind11)

rawDB предоставляет официальные биндинги для Python, созданные с использованием `pybind11`. Это позволяет использовать базу данных со скоростью C++ (2.2M RPS) прямо из Python скриптов, минуя медленные IPC вызовы или HTTP запросы.

## Компиляция модуля

При сборке через CMake, модуль собирается автоматически (в файл `rawdb.cpython-*.so` или `rawdb.pyd`).
Просто добавьте директорию сборки в `sys.path` или установите модуль через `pip` / `setuptools` (если вы настроили `setup.py`).

## API Классов

В Python прокинуты те же высокоуровневые сущности, что и в C++:
- `rawdb.Database()`
- `rawdb.Connection(db)`
- `rawdb.Executor(conn)`

## Пример использования

Главная особенность Python API в том, что метод `execute()` возвращает не сырые строки, а удобный список словарей (`list` of `dict`).

```python
import rawdb

# 1. Открытие БД
db = rawdb.Database()
db.open("/tmp/rawdb_python_data")

# 2. Создание сессии
conn = rawdb.Connection(db)
exec = rawdb.Executor(conn)

# 3. Выполнение миграций / вставок с транзакциями
conn.begin()
exec.execute("CREATE TABLE users (id INT64, name VARCHAR, balance FLOAT64)")
exec.execute("INSERT INTO users VALUES (1, 'Alice', 1500.50)")
exec.execute("INSERT INTO users VALUES (2, 'Bob', 200.00)")
exec.execute("INSERT INTO users VALUES (3, 'Alice', 500.00)")
conn.commit()

# 4. Аналитические запросы (SELECT)
conn.begin()
# Простая выборка
result = exec.execute("SELECT * FROM users")
print(result)
# [{'id': 1, 'name': 'Alice', 'balance': 1500.5}, {'id': 2, 'name': 'Bob', 'balance': 200.0}, ...]

# Агрегации и GROUP BY
agg_result = exec.execute("SELECT name, COUNT(*), SUM(balance) FROM users GROUP BY name")
print(agg_result)
# [{'name': 'Alice', 'COUNT(*)': 2, 'SUM(balance)': 2000.5}, {'name': 'Bob', 'COUNT(*)': 1, 'SUM(balance)': 200.0}]
conn.commit()

# 5. Закрытие (Важно для flush'а данных и завершения WAL потоков)
db.close()
```

## Безопасность типов

При возвращении результатов из `execute()`, C++ типы автоматически конвертируются в нативные Python-типы:
- `INT64` -> `int`
- `FLOAT64` -> `float`
- `VARCHAR` -> `str`
- `BOOL` -> `bool`
