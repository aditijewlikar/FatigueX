import re
import codecs

filepath = r'C:\Users\disha\Downloads\aditi\FatigueX\V2\backend\server.js'
with codecs.open(filepath, 'r', encoding='utf-8') as f:
    js = f.read()

# Replace 'public' with '../frontend'
js = re.sub(r"app\.use\(express\.static\(path\.join\(__dirname,\s*'public'\)\)\);", 
            "app.use(express.static(path.join(__dirname, '../frontend')));", js)

with codecs.open(filepath, 'w', encoding='utf-8') as f:
    f.write(js)
print("Patched server.js to serve from ../frontend")
