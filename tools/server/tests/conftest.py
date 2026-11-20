import os

import pytest

from utils import server_instances


@pytest.fixture(scope="session", autouse=True)
def configure_worker_port(request):
    # one port range per xdist worker
    worker_id = getattr(request.config, "workerinput", {}).get("workerid", "master")
    worker_num = 0 if worker_id == "master" else int(worker_id[2:])
    port_base = int(os.environ.get("IXIAN_SERVER_TEST_PORT_BASE", os.environ.get("LLAMA_SERVER_TEST_PORT_BASE", "9931")))
    os.environ["PORT"] = str(port_base + worker_num * 10)


@pytest.fixture(autouse=True)
def stop_server_after_each_test():
    yield
    for server in set(server_instances):
        server.stop()
