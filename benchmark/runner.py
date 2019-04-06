import itertools
import os
import subprocess
from datetime import datetime
from typing import Sequence, Mapping, Any, Tuple, Union, List
from termcolor import colored


class BenchmarkApp:
    def __init__(self,
                 app_path='./cmake-build-release/lru_benchmark',
                 log_file='default.csv',
                 run_name='main',
                 run_info='',
                 generator='same',
                 backend='hash',
                 payload_level=28,
                 threads=1,
                 iterations=1000000000,
                 limit_max_key=False,
                 is_item_capacity=True,
                 capacity=100,
                 pull_threshold=0.5,
                 purge_threshold=0.01,
                 verbose=True,
                 print_freq=1000,
                 time_limit=10,
                 reps=3):
        self.app_path = app_path
        self.log_file = log_file
        self.run_name = run_name
        self.run_info = run_info
        self.generator = generator
        self.backend = backend
        self.payload_level = payload_level
        self.threads = threads
        self.iterations = iterations
        self.limit_max_key = limit_max_key
        self.is_item_capacity = is_item_capacity
        self.capacity = capacity
        self.pull_threshold = pull_threshold
        self.purge_threshold = purge_threshold
        self.ve100000rbose = verbose
        self.print_freq = print_freq
        self.time_limit = time_limit
        self.reps = reps
        print(colored(f'{log_file}|{run_name}|{run_info}', 'green', attrs=['bold']))

    def run(self, overrides: Sequence[Tuple[Union[str, List[str]], Union[List[Any], List[List[Any]]]]] = None,
            changes=[]):
        def wrap_list(x):
            return x if isinstance(x, list) else [x]

        old = self.__dict__

        if not overrides:
            print(colored('\n>> ' + ', '.join(changes), 'blue', attrs=['bold']))
            self.execute_benchmark()
        else:
            this_level = overrides[0]
            if not isinstance(this_level[0], list):
                this_level = ([this_level[0]], [[x] for x in wrap_list(this_level[1])])
            keys, values = this_level
            for vv in values:
                for k, v in zip(keys, vv):
                    setattr(self, k, v)
                self.run(overrides[1:], changes + [f'{k}={v}' for k, v in zip(keys, vv)])

        self.__dict__ = old

    def execute_benchmark(self):
        args = [self.app_path,
                '-L', self.log_file,
                '-N', self.run_name,
                '-I', self.run_info,
                '-G', self.generator,
                '-v',
                '-B', self.backend,
                '-t', self.threads,
                '-c' if self.is_item_capacity else '-m', self.capacity,
                '-i', self.iterations,
                '-q', self.print_freq,
                '-p', self.payload_level,
                '--pull-thrs', self.pull_threshold,
                '--purge-thrs', self.purge_threshold,
                '--time-limit', self.time_limit
                ]
        if self.limit_max_key:
            args.append('--fix-max-key')
            args.append('1')

        args = [str(a) for a in args]
        print('  >> ' + ' '.join(args))
        try:
            subprocess.run(args)
        except subprocess.CalledProcessError as e:
            print(e, file=os.stderr)


def get_run_name():
    return datetime.now().strftime('%H:%M:%S')


TIME_LIMIT = 70
THREADS = [1, 2, 4, 8, 16, 32]
ALL_CONTAINERS = ["tbb_hash", "lru", "concurrent", "deferred", "tbb",
                  "hhvm", "b_lru", "b_concurrent", "b_deferred"]

LRU_CONTAINERS = ALL_CONTAINERS[1:]
BUCKET_LRU_CONTAINERS = ALL_CONTAINERS[5:]


def find_performance():
    app = BenchmarkApp(log_file='data.csv', run_name=get_run_name(), run_info='find',
                       capacity=3000 * 1000, time_limit=TIME_LIMIT, print_freq=10000000,
                       payload_level=5, limit_max_key=True)
    app.run([
        ('reps', [1, 2, 3]),
        ('generator', ['uniform', 'normal']),
        ('threads', THREADS),
        ('backend', ALL_CONTAINERS),
    ])


def lru_performance():
    app = BenchmarkApp(log_file='data.csv', run_name=get_run_name(), run_info='lru',
                       capacity=1000 * 1000, time_limit=TIME_LIMIT, print_freq=1000000,
                       payload_level=5, purge_threshold=0.1)
    app.run([
        ('reps', [1, 2, 3]),
        ('generator', ['same', 'disjoint']),
        ('threads', THREADS),
        ('backend', LRU_CONTAINERS),
    ])


def simulation_performance():
    app = BenchmarkApp(log_file='data.csv', run_name=get_run_name(), run_info='simul',
                       capacity=1000 * 1000, time_limit=TIME_LIMIT, print_freq=100000,
                       payload_level=5, limit_max_key=True, purge_threshold=0.3, pull_threshold=0.05)
    app.run([
        ('reps', [1, 2, 3]),
        ('generator', ['exp']),
        ('threads', THREADS),
        ('backend', LRU_CONTAINERS),
    ])


if __name__ == '__main__':
    all_tests = [find_performance, lru_performance, simulation_performance]
    for t in all_tests:
        t()
