# What an agent's tests of a small project ask of unittest, argparse, pathlib
# and the like (app/python_train.sh).
import argparse
import collections
import csv
import io
import pathlib
import tempfile
import textwrap
import unittest


class Session(unittest.TestCase):
    def test_parse(self):
        parser = argparse.ArgumentParser()
        parser.add_argument('--limit', type=int, default=10)
        parser.add_argument('files', nargs='*')
        self.assertEqual(parser.parse_args(['--limit', '20', 'a', 'b']).limit, 20)

    def test_files(self):
        with tempfile.TemporaryDirectory() as d:
            for i in range(50):
                pathlib.Path(d, 'f%d.py' % i).write_text('value = %d\n' % i)
            found = sorted(p.name for p in pathlib.Path(d).glob('*.py'))
            self.assertEqual(len(found), 50)

    def test_csv(self):
        out = io.StringIO()
        csv.writer(out).writerows([[i, i * i, 'x' * i] for i in range(200)])
        rows = list(csv.reader(io.StringIO(out.getvalue())))
        self.assertEqual(collections.Counter(len(r) for r in rows)[3], 200)

    def test_text(self):
        self.assertIn('limit', textwrap.fill('raise the limit ' * 40, 60))


if __name__ == '__main__':
    unittest.main()
