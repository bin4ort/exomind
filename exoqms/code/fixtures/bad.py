def parse(data):
    try:
        return json.loads(data)
    except:
        return None

def collect(items=[]):
    items.append(1)
    return items

assert len(data) > 0
os.system("rm -rf /tmp/x")
