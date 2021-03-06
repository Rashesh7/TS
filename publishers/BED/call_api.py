#!/usr/bin/env python
# Copyright (C) 2011 Ion Torrent Systems, Inc. All Rights Reserved
# Author: Brian Kennedy

import json
import httplib2
import urllib

url = "http://localhost/rundb/api/v1/%s/"
uri = "http://localhost/rundb/api/v1/%s/%s/"
headers = {"Content-type": "application/json"}


def get(where, **query):
    """Returns a JSON API result object."""
    body = urllib.urlencode(query)
    query_string = "%s?%s" % (url % where, body)
    h = httplib2.Http()
    response, content = h.request(query_string, method="GET", body=body, headers=headers)
    return json.loads(content), response, content


def post(where, **query):
    """Returns the API URI for the newly created item."""
    body = json.dumps(query)
    item_url = url % where
    h = httplib2.Http()
    response, content = h.request(item_url, method="POST", body=body, headers=headers)
    return response["status"] == "201", response, content


def put(where, item_id, **update):
    """Returns True if successful; otherwise, False"""
    body = json.dumps(update)
    item_uri = uri % (where, str(item_id))
    h = httplib2.Http()
    response, content = h.request(item_uri, method="PUT", body=body, headers=headers)
    return response["status"] == "204", response, content


def patch(where, item_id, **update):
    """Returns True if successful; otherwise, False"""
    body = json.dumps(update)
    item_uri = uri % (where, str(item_id))
    h = httplib2.Http()
    response, content = h.request(item_uri, method="PATCH", body=body, headers=headers)
    return response["status"] == "202", response, content


def delete(where, item_id):
    """Returns True if successful; otherwise, False"""
    item_uri = uri % (where, str(item_id))
    h = httplib2.Http()
    response, content = h.request(item_uri, method="DELETE", headers=headers)
    return response["status"] == "204", response, content
