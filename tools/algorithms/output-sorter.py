import pandas as pd
import sys


# Input and output file paths
input_file = sys.argv[1]
output_file = sys.argv[1]



# Read CSV
df = pd.read_csv(input_file)

# Sort by depth first, then Node
df_sorted = df.sort_values(by=["depth", "uuid"], ascending=[True, True])

# Save to new CSV (without index column)
df_sorted.to_csv(output_file, index=False)

print(f"Sorted CSV saved to {output_file}")