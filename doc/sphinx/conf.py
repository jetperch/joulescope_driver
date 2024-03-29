# Configuration file for the Sphinx documentation builder.
#
# This file only contains a selection of the most common options. For a full
# list see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

# -- Path setup --------------------------------------------------------------

# If extensions (or modules to document with autodoc) are in another directory,
# add these directories to sys.path here. If the directory is relative to the
# documentation root, use os.path.abspath to make it absolute, like shown here.
#
import os
import sys
DOC_SPHINX_PATH = os.path.dirname(os.path.abspath(__file__))
PROJ_PATH = os.path.dirname(os.path.dirname(DOC_SPHINX_PATH))
sys.path.insert(0, PROJ_PATH)
import pyjoulescope_driver


# -- Project information -----------------------------------------------------

project = pyjoulescope_driver.__title__
copyright = pyjoulescope_driver.__copyright__
author = pyjoulescope_driver.__author__
version = pyjoulescope_driver.__version__
release = pyjoulescope_driver.__version__

read_the_docs_build = os.environ.get('READTHEDOCS', None) == 'True'

# -- General configuration ---------------------------------------------------

master_doc = 'index'

# Add any Sphinx extension module names here, as strings. They can be
# extensions coming with Sphinx (named 'sphinx.ext.*') or your custom
# ones.
extensions = [
    "sphinx.ext.autodoc",
    "sphinx.ext.intersphinx",
    "sphinx.ext.todo",
    "sphinx.ext.viewcode",
    'myst_parser',
    'breathe',
]

source_suffix = {
    '.rst': 'restructuredtext',
    '.txt': 'markdown',
    '.md': 'markdown',
}

# Add any paths that contain templates here, relative to this directory.
templates_path = ['_templates']

# List of patterns, relative to source directory, that match files and
# directories to ignore when looking for source files.
# This pattern also affects html_static_path and html_extra_path.
exclude_patterns = []


# -- Options for HTML output -------------------------------------------------

# The theme to use for HTML and HTML Help pages.  See the documentation for
# a list of builtin themes.
#
html_theme = 'sphinx_rtd_theme'

# Theme options are theme-specific and customize the look and feel of a theme
# further.  For a list of options available for each theme, see the
# documentation.
html_theme_options = {}


# Add any paths that contain custom static files (such as style sheets) here,
# relative to this directory. They are copied after the builtin static files,
# so a file named "default.css" will overwrite the builtin "default.css".
html_static_path = ['_static']

html_logo = "_static/icon_1024x1024.png"
html_favicon = "_static/favicon.ico"


# Breathe Configuration
breathe_default_project = "jsdrv"
breathe_projects = {"jsdrv": os.path.join(PROJ_PATH, 'cmake-build', 'doc', 'doxygen', 'xml')}


def setup(app):
    pass
