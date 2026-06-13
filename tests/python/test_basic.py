import os
import sys

# Assume the shared library is built in the build/ directory
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '../../build')))

import rawdb

def test_rawdb():
    print("--- Starting Python Bindings Test ---")
    db = rawdb.Database()
    
    # Use a temporary directory for the DB
    db_path = "/tmp/rawdb_python_test"
    import shutil
    if os.path.exists(db_path):
        shutil.rmtree(db_path)
        
    db.open(db_path)
    print("Database opened successfully.")
    
    conn = rawdb.Connection(db)
    exec = rawdb.Executor(conn)
    
    conn.begin()
    exec.execute("CREATE TABLE users (id INT64, name VARCHAR, balance FLOAT64)")
    exec.execute("INSERT INTO users VALUES (1, 'alice', 1500.50)")
    exec.execute("INSERT INTO users VALUES (2, 'bob', 200.00)")
    conn.commit()
    print("Data inserted and committed.")
    
    conn.begin()
    result = exec.execute("SELECT * FROM users")
    conn.commit()
    
    print("Query Result:", result)
    assert len(result) == 2, "Expected 2 rows"
    
    # Test dictionary conversion
    assert result[0]['id'] == 1
    assert result[0]['name'] == 'alice'
    assert result[0]['balance'] == 1500.5
    
    # Test aggregations
    conn.begin()
    agg_result = exec.execute("SELECT COUNT(*), SUM(balance), MAX(balance) FROM users")
    conn.commit()
    print("Aggregation Result:", agg_result)
    assert float(agg_result[0]['COUNT(*)']) == 2
    assert float(agg_result[0]['SUM(balance)']) == 1700.5
    assert float(agg_result[0]['MAX(balance)']) == 1500.5
    
    # Test Group By
    conn.begin()
    exec.execute("INSERT INTO users VALUES (3, 'alice', 500.00)")
    gb_result = exec.execute("SELECT name, COUNT(*), SUM(balance) FROM users GROUP BY name")
    conn.commit()
    print("Group By Result:", gb_result)
    
    alice_row = next(r for r in gb_result if r['name'] == 'alice')
    bob_row = next(r for r in gb_result if r['name'] == 'bob')
    
    assert float(alice_row['COUNT(*)']) == 2
    assert float(alice_row['SUM(balance)']) == 2000.5
    assert float(bob_row['COUNT(*)']) == 1
    assert float(bob_row['SUM(balance)']) == 200.0
    
    print("Python dict conversion and AGGREGATIONS are working correctly!")
    
    db.close()
    print("Database closed. Test passed.")

if __name__ == "__main__":
    test_rawdb()
