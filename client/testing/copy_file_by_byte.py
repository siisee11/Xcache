# read_big_file.py

#file_name = "hihi.txt"
#file_name = "2Gdummyfile"
file_name = "foo"

fr = open(file_name, 'r')
fw = open(file_name + ".copy", 'w')

while (byte := fr.read(1)):
    fw.write(line)
fr.close()
fw.close()
