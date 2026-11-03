"""Background jobs for long-running operations.

HTTP requests never block for long: they start a job, return ``202`` with
the job id, and the browser follows progress via ``GET /api/v1/jobs/{id}``
or the live event stream.
"""

from __future__ import annotations

import asyncio
import logging
import secrets
import time
from collections import OrderedDict
from typing import Any, Awaitable, Callable, Dict, Optional

from .control_client import OutcomeUnknown

log = logging.getLogger("waver_web.jobs")


class Job:
    def __init__(self, kind: str, user: str, resource: str) -> None:
        self.id = secrets.token_hex(8)
        self.kind = kind
        self.user = user
        self.resource = resource
        self.state = "queued"  # queued, running, succeeded, failed, uncertain, cancelled
        self.progress = 0
        self.message = ""
        self.result: Any = None
        self.error: Optional[Dict[str, str]] = None
        self.created = time.time()
        self.finished: Optional[float] = None
        self.task: Optional[asyncio.Task] = None
        self.on_finished: Optional[Callable[["Job"], None]] = None

    def to_dict(self) -> Dict[str, Any]:
        return {"id": self.id, "kind": self.kind, "user": self.user, "resource": self.resource,
                "state": self.state, "progress": self.progress, "message": self.message,
                "result": self.result, "error": self.error, "created": self.created,
                "finished": self.finished}


class JobContext:
    def __init__(self, manager: "JobManager", job: Job) -> None:
        self._manager = manager
        self.job = job

    def progress(self, percent: int, message: str) -> None:
        self.job.progress = max(0, min(100, int(percent)))
        self.job.message = message
        self._manager._notify(self.job)


JobFn = Callable[[JobContext], Awaitable[Any]]


class JobManager:
    def __init__(self, publish: Callable[[str, Dict[str, Any]], None], keep: int = 100,
                 timeout: float = 120.0, on_finished: Optional[Callable[[Job], None]] = None) -> None:
        self._jobs: "OrderedDict[str, Job]" = OrderedDict()
        self._publish = publish
        self.keep = keep
        self.timeout = timeout
        self._on_finished = on_finished
        self._exclusive: Dict[str, str] = {}

    def _notify(self, job: Job) -> None:
        self._publish("job.updated", job.to_dict())

    def running(self, exclusive_key: str) -> Optional[Job]:
        jid = self._exclusive.get(exclusive_key)
        job = self._jobs.get(jid) if jid else None
        if job and job.state in ("queued", "running"):
            return job
        return None

    def start(self, kind: str, user: str, resource: str, fn: JobFn, exclusive_key: Optional[str] = None,
              on_finished: Optional[Callable[[Job], None]] = None) -> Job:
        job = Job(kind, user, resource)
        job.on_finished = on_finished
        self._jobs[job.id] = job
        while len(self._jobs) > self.keep:
            oldest = next(iter(self._jobs))
            if self._jobs[oldest].state in ("queued", "running"):
                break
            self._jobs.popitem(last=False)
        if exclusive_key:
            self._exclusive[exclusive_key] = job.id
        job.task = asyncio.ensure_future(self._run(job, fn))
        self._notify(job)
        return job

    async def _run(self, job: Job, fn: JobFn) -> None:
        job.state = "running"
        self._notify(job)
        ctx = JobContext(self, job)
        try:
            job.result = await asyncio.wait_for(fn(ctx), self.timeout)
            job.state = "succeeded"
            job.progress = 100
        except asyncio.CancelledError:
            job.state = "cancelled"
            job.error = {"code": "cancelled", "message": "job was cancelled"}
        except asyncio.TimeoutError:
            job.state = "uncertain"
            job.error = {"code": "timeout", "message": "the operation did not finish in time; refresh the status"}
        except OutcomeUnknown as e:
            job.state = "uncertain"
            job.error = {"code": "outcome_unknown", "message": str(e)}
        except JobFailure as e:
            job.state = "failed"
            job.error = {"code": e.code, "message": e.message}
        except Exception as e:  # pragma: no cover - defensive
            log.exception("job %s failed", job.kind)
            job.state = "failed"
            job.error = {"code": "internal", "message": str(e) or type(e).__name__}
        job.finished = time.time()
        if job.state != "succeeded" and not job.message:
            job.message = job.error["message"] if job.error else ""
        self._notify(job)
        for hook in (job.on_finished, self._on_finished):
            if hook:
                try:
                    hook(job)
                except Exception:  # pragma: no cover
                    log.exception("job finished hook failed")

    def get(self, job_id: str) -> Optional[Job]:
        return self._jobs.get(job_id)

    def list(self):
        return [j.to_dict() for j in reversed(list(self._jobs.values()))]

    async def shutdown(self, grace: float = 5.0) -> None:
        tasks = [j.task for j in self._jobs.values() if j.task and not j.task.done()]
        if not tasks:
            return
        done, pending = await asyncio.wait(tasks, timeout=grace)
        for t in pending:
            t.cancel()
        if pending:
            await asyncio.wait(pending, timeout=1.0)


class JobFailure(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code
        self.message = message
