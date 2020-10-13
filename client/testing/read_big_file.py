# read_big_file.py

#file_name = "hihi.txt"
#file_name = "2Gdummyfile"
file_name = "/test/4Gdummyfile"

fr = open(file_name, 'r')
fw = open(file_name + ".copy", 'w')

while True:
    line = fr.readline()
    if not line: break
    fw.write(line)
fr.close()
fw.close()
