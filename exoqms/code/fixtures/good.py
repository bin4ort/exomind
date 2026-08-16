def parse(data):
    try:
        return json.loads(data)
    except (ValueError, TypeError) as e:
        return None

def collect(items=None):
    if items is None:
        items = []
    items.append(1)
    return items
