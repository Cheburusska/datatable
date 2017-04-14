#!/usr/bin/env python3
# Copyright 2017 H2O.ai; Apache License Version 2.0;  -*- encoding: utf-8 -*-

from .__version__ import version as __version__
from .dt import DataTable
from .memmap import open

__all__ = ("__version__", "DataTable", "open")


DataTable.__module__ = "datatable"
