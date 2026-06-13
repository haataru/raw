# Поддерживаемый диалект SQL

rawDB предоставляет полноценный парсер SQL. Синтаксис поддерживает как транзакционную работу, так и аналитические агрегации. Ключевые слова регистронезависимы, точка с запятой `;` в конце запроса опциональна.

## Типы данных

- `INT64` — 64-битное целое число со знаком.
- `FLOAT64` — 64-битное число с плавающей точкой (double).
- `VARCHAR` — строки переменной длины.
- `BOOL` — булево значение (`true`, `false`, `1`, `0`).

## Транзакции

Поддерживается классическое ACID управление транзакциями:
```sql
BEGIN;
-- ваши запросы --
COMMIT;
-- или ROLLBACK;
```

## DDL (Data Definition Language)

**Создание таблицы**
```sql
CREATE TABLE users (id INT64, name VARCHAR, balance FLOAT64);
```

**Создание B-Tree индекса**
Индексы строятся для быстрого точечного поиска (`WHERE col = val`).
```sql
CREATE INDEX idx_users_id ON users (id);
```

**Оптимизация и сжатие (VACUUM)**
Освобождает место от удаленных записей и перестраивает индексы.
```sql
VACUUM users;
```

## DML (Data Manipulation Language)

**Вставка данных**
```sql
INSERT INTO users VALUES (1, 'Alice', 1500.50), (2, 'Bob', 200.0);
```

**Обновление данных**
```sql
UPDATE users SET balance = 2000.0 WHERE name = 'Alice';
```

**Удаление данных**
```sql
DELETE FROM users WHERE balance < 0;
```

## Аналитика и DQL (Data Query Language)

rawDB поддерживает мощный `SELECT` с агрегациями и группировкой:

**Простая выборка**
```sql
SELECT id, name FROM users WHERE balance >= 1000 ORDER BY balance DESC LIMIT 10;
```

**Агрегатные функции**
Поддерживаются классические агрегации: `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`.
```sql
SELECT COUNT(*), SUM(balance), MAX(balance) FROM users;
```

**Группировка (GROUP BY)**
```sql
SELECT department, COUNT(*), AVG(salary) FROM employees GROUP BY department;
```

### Поддерживаемые операторы сравнения
В блоке `WHERE` доступны: `=`, `!=`, `<`, `<=`, `>`, `>=`.
Множественные условия поддерживаются через `AND` (оператор `OR` находится в разработке).
