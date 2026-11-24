# libpython for the app's tools: what an agent's session runs in python3, the
# standard library only (the app's root has no more): starting, json, re,
# base64 and sqlite3, on the inputs of ../py/gen_data.py.
python3 /tmp/aojit/py/gen_data.py
for t in 01_startup 02_json_stdin_tail 03_json_file_key 04_json_big 05_log_scan 06_base64_json 11_sqlite; do
    sh "/tmp/aojit/py/tasks/$t.sh" > /dev/null
done
python3 -m unittest -q /tmp/aojit/app/python_unittest.py 2>&1 | tail -1
